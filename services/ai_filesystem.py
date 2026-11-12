"""
=============================================================================
JARVIS AI OS — AI File System Service  (services/ai_filesystem.py)
Language: Python 3.11+
Runs as: privileged kernel-adjacent Python daemon (via embedded CPython)

Provides AI-enhanced file operations:
  - Natural language search ("find my resume from last week")
  - Smart organisation (auto-classify and move files by content)
  - Semantic deduplication (find near-duplicate files)
  - AI-generated file summaries on access
  - Voice-driven file management

[MODEL_SLOT:FILE_EMBEDDER]
  Document embedding model for semantic file search.
  Options:
    - all-MiniLM-L6-v2  (HuggingFace: sentence-transformers/all-MiniLM-L6-v2)
    - bge-small-en-v1.5 (HuggingFace: BAAI/bge-small-en-v1.5)
  Place in: /jarvis/models/filesystem/embedder/
  These are small (22–135 MB) and run fast on CPU.

[MODEL_SLOT:FILE_CLASSIFIER]
  Zero-shot document classifier.
  Options:
    - facebook/bart-large-mnli  (zero-shot, no fine-tuning needed)
    - typeform/distilbart-mnli-12-3  (smaller, faster)
  Used to auto-tag files: invoice, contract, photo, code, report, etc.
=============================================================================
"""

import os
import re
import json
import time
import hashlib
import struct
import mmap
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional

# ---------------------------------------------------------------------------
# These imports are available when running inside JARVIS OS user space.
# In the stub, we guard them — the real daemon has them installed.
# ---------------------------------------------------------------------------
try:
    from sentence_transformers import SentenceTransformer
    import numpy as np
    HAS_EMBEDDER = True
except ImportError:
    HAS_EMBEDDER = False
    print("[FS] WARNING: sentence-transformers not installed. Semantic search disabled.")

try:
    from transformers import pipeline
    HAS_CLASSIFIER = True
except ImportError:
    HAS_CLASSIFIER = False

# ---------------------------------------------------------------------------
# IPC bridge to kernel (shared memory ring buffer via /dev/jarvis_ipc)
# The kernel exposes a character device; Python reads/writes JSON messages.
# ---------------------------------------------------------------------------
IPC_DEV       = "/dev/jarvis_ipc"
MODEL_BASE    = "/jarvis/models/filesystem"
JARVIS_ROOT   = "/jarvis"
INDEX_PATH    = "/jarvis/data/file_index.json"
LOG_PATH      = "/jarvis/logs/filesystem.log"

# ---------------------------------------------------------------------------
# File record — stored in the semantic index
# ---------------------------------------------------------------------------
@dataclass
class FileRecord:
    path:        str
    size:        int
    mtime:       float
    content_hash:str
    mime_type:   str
    tags:        list[str]    = field(default_factory=list)
    summary:     str          = ""
    embedding:   list[float]  = field(default_factory=list)


# ===========================================================================
# Semantic File Index
# Maintains an in-memory embedding index of all files for fast vector search.
# Persisted to /jarvis/data/file_index.json between reboots.
# ===========================================================================
class SemanticFileIndex:
    def __init__(self):
        self.records:  dict[str, FileRecord] = {}  # path → FileRecord
        self.embeddings: list[tuple[str, list[float]]] = []  # for cosine search

    def load_from_disk(self, index_path: str):
        """Load persisted index."""
        try:
            with open(index_path, "r") as f:
                data = json.load(f)
            for rec in data:
                r = FileRecord(**rec)
                self.records[r.path] = r
            print(f"[FS] Loaded {len(self.records)} file records from index")
        except FileNotFoundError:
            print("[FS] No existing index found — will build fresh")
        except Exception as e:
            print(f"[FS] Index load error: {e}")

    def save_to_disk(self, index_path: str):
        """Persist index."""
        os.makedirs(os.path.dirname(index_path), exist_ok=True)
        data = [r.__dict__ for r in self.records.values()]
        with open(index_path, "w") as f:
            json.dump(data, f, indent=2)

    def add_or_update(self, record: FileRecord):
        self.records[record.path] = record
        if record.embedding:
            # Replace existing embedding
            self.embeddings = [(p, e) for p, e in self.embeddings if p != record.path]
            self.embeddings.append((record.path, record.embedding))

    def cosine_similarity(self, a: list[float], b: list[float]) -> float:
        if not HAS_EMBEDDER:
            return 0.0
        import numpy as np
        va, vb = np.array(a), np.array(b)
        denom = (np.linalg.norm(va) * np.linalg.norm(vb))
        if denom < 1e-9:
            return 0.0
        return float(np.dot(va, vb) / denom)

    def semantic_search(self, query_embedding: list[float], top_k: int = 10) -> list[tuple[str, float]]:
        """Return top_k (path, score) sorted by cosine similarity."""
        scores = [
            (path, self.cosine_similarity(query_embedding, emb))
            for path, emb in self.embeddings
        ]
        scores.sort(key=lambda x: x[1], reverse=True)
        return scores[:top_k]


# ===========================================================================
# AI File System Service
# ===========================================================================
class AIFilesystemService:

    def __init__(self):
        self.index     = SemanticFileIndex()
        self.embedder  = None   # [MODEL_SLOT:FILE_EMBEDDER]
        self.classifier= None   # [MODEL_SLOT:FILE_CLASSIFIER]
        self.running   = False

    # -----------------------------------------------------------------------
    # Initialise models
    # -----------------------------------------------------------------------
    def init(self):
        print("[FS] Initialising AI filesystem service")
        self.index.load_from_disk(INDEX_PATH)

        # Load embedder
        # [MODEL_SLOT:FILE_EMBEDDER]
        # Drop model in /jarvis/models/filesystem/embedder/
        embedder_path = os.path.join(MODEL_BASE, "embedder")
        if HAS_EMBEDDER and os.path.isdir(embedder_path):
            print(f"[FS] Loading embedder: {embedder_path}")
            self.embedder = SentenceTransformer(embedder_path)
            print("[FS] Embedder loaded")
        elif HAS_EMBEDDER:
            # Fallback: download from HuggingFace on first run
            print("[FS] Downloading all-MiniLM-L6-v2 from HuggingFace...")
            self.embedder = SentenceTransformer("all-MiniLM-L6-v2")

        # Load zero-shot classifier
        # [MODEL_SLOT:FILE_CLASSIFIER]
        classifier_path = os.path.join(MODEL_BASE, "classifier")
        if HAS_CLASSIFIER and os.path.isdir(classifier_path):
            print(f"[FS] Loading classifier: {classifier_path}")
            self.classifier = pipeline("zero-shot-classification",
                                       model=classifier_path,
                                       device=-1)  # -1 = CPU, 0 = first GPU
        elif HAS_CLASSIFIER:
            print("[FS] Loading bart-large-mnli from HuggingFace...")
            self.classifier = pipeline("zero-shot-classification",
                                       model="facebook/bart-large-mnli",
                                       device=-1)

        print("[FS] AI filesystem service ready")

    # -----------------------------------------------------------------------
    # Index a single file
    # -----------------------------------------------------------------------
    def index_file(self, path: str) -> Optional[FileRecord]:
        try:
            stat  = os.stat(path)
            mtype = self._detect_mime(path)
            text  = self._extract_text(path, mtype)

            # Hash content for deduplication
            h = hashlib.sha256()
            with open(path, "rb") as f:
                for chunk in iter(lambda: f.read(65536), b""):
                    h.update(chunk)
            chash = h.hexdigest()

            # Generate embedding
            embedding = []
            if self.embedder and text:
                embedding = self.embedder.encode(text[:2000]).tolist()

            # Classify
            tags = []
            if self.classifier and text:
                labels = ["invoice", "contract", "report", "code", "image",
                          "video", "spreadsheet", "presentation", "email",
                          "note", "receipt", "config"]
                result = self.classifier(text[:512], labels, multi_label=True)
                tags = [l for l, s in zip(result["labels"], result["scores"]) if s > 0.5]

            # Quick summary (first 200 chars of extracted text)
            summary = text[:200].replace("\n", " ").strip() if text else ""

            record = FileRecord(
                path         = path,
                size         = stat.st_size,
                mtime        = stat.st_mtime,
                content_hash = chash,
                mime_type    = mtype,
                tags         = tags,
                summary      = summary,
                embedding    = embedding,
            )
            self.index.add_or_update(record)
            return record

        except PermissionError:
            return None
        except Exception as e:
            print(f"[FS] Index error for {path}: {e}")
            return None

    # -----------------------------------------------------------------------
    # Natural language file search
    # Called by AI brain via tool: search_files
    # -----------------------------------------------------------------------
    def natural_language_search(self, query: str, top_k: int = 10) -> list[dict]:
        """
        Examples:
          "find my resume"
          "Python files modified this week"
          "invoices from January"
          "photos of the beach"
        """
        results = []

        # Parse time filters
        time_filter = self._parse_time_filter(query)

        # Keyword fallback
        keyword = re.sub(r'\b(find|search|look for|show|open|my|the|a|an)\b',
                         '', query, flags=re.IGNORECASE).strip()

        if self.embedder:
            query_emb = self.embedder.encode(keyword).tolist()
            matches   = self.index.semantic_search(query_emb, top_k=top_k * 3)
        else:
            # Fall back to filename keyword search
            matches = []
            kl = keyword.lower()
            for path in self.index.records:
                if kl in os.path.basename(path).lower():
                    matches.append((path, 1.0))

        for path, score in matches:
            rec = self.index.records.get(path)
            if not rec:
                continue
            # Apply time filter
            if time_filter and rec.mtime < time_filter:
                continue
            # Check file still exists
            if not os.path.exists(path):
                continue
            results.append({
                "path":    path,
                "score":   round(score, 3),
                "tags":    rec.tags,
                "summary": rec.summary,
                "size":    rec.size,
                "mtime":   rec.mtime,
            })
            if len(results) >= top_k:
                break

        return results

    # -----------------------------------------------------------------------
    # Auto-organise: scan Downloads and sort by AI tags
    # -----------------------------------------------------------------------
    def auto_organise(self, source_dir: str, dest_dir: str):
        """
        Scans source_dir, classifies each file, moves to:
          dest_dir/invoices/, dest_dir/code/, dest_dir/images/, etc.
        """
        print(f"[FS] Auto-organising {source_dir} → {dest_dir}")
        for fname in os.listdir(source_dir):
            fpath = os.path.join(source_dir, fname)
            if not os.path.isfile(fpath):
                continue
            record = self.index_file(fpath)
            if not record or not record.tags:
                tag = "misc"
            else:
                tag = record.tags[0]

            target_dir = os.path.join(dest_dir, tag)
            os.makedirs(target_dir, exist_ok=True)
            target = os.path.join(target_dir, fname)
            os.rename(fpath, target)
            print(f"[FS] {fname} → {tag}/")

    # -----------------------------------------------------------------------
    # Semantic deduplication
    # -----------------------------------------------------------------------
    def find_duplicates(self) -> list[list[str]]:
        """Group files by content_hash first, then near-duplicate by embedding."""
        # Exact duplicates
        by_hash: dict[str, list[str]] = {}
        for path, rec in self.index.records.items():
            by_hash.setdefault(rec.content_hash, []).append(path)
        dupes = [paths for paths in by_hash.values() if len(paths) > 1]

        # Near-duplicates via embedding cosine similarity > 0.97
        if self.embedder:
            paths  = [p for p, r in self.index.records.items() if r.embedding]
            embs   = [self.index.records[p].embedding for p in paths]
            for i in range(len(paths)):
                group = [paths[i]]
                for j in range(i + 1, len(paths)):
                    sim = self.index.cosine_similarity(embs[i], embs[j])
                    if sim > 0.97:
                        group.append(paths[j])
                if len(group) > 1:
                    dupes.append(group)

        return dupes

    # -----------------------------------------------------------------------
    # IPC event loop — listens for FILE_OP messages from kernel
    # -----------------------------------------------------------------------
    def run(self):
        self.running = True
        print("[FS] Entering IPC event loop")

        # Initial scan of user home
        self._background_index_scan("/home")

        # Listen for kernel IPC messages
        try:
            ipc = open(IPC_DEV, "rb") if os.path.exists(IPC_DEV) else None
        except Exception:
            ipc = None
            print("[FS] IPC device not available — running in standalone mode")

        while self.running:
            if ipc:
                msg = self._read_ipc_message(ipc)
                if msg:
                    self._handle_message(msg)
            time.sleep(0.05)  # 50 ms poll

    def _handle_message(self, msg: dict):
        mtype = msg.get("type")
        payload = msg.get("payload", "")

        if mtype == "FILE_SEARCH":
            results = self.natural_language_search(payload)
            self._send_ipc_response("FILE_SEARCH_RESULT", json.dumps(results))

        elif mtype == "FILE_INDEX":
            self.index_file(payload)

        elif mtype == "AUTO_ORGANISE":
            parts = payload.split("→")
            if len(parts) == 2:
                self.auto_organise(parts[0].strip(), parts[1].strip())

        elif mtype == "FIND_DUPES":
            dupes = self.find_duplicates()
            self._send_ipc_response("DUPE_RESULT", json.dumps(dupes))

    # -----------------------------------------------------------------------
    # Helpers
    # -----------------------------------------------------------------------
    def _detect_mime(self, path: str) -> str:
        ext = Path(path).suffix.lower()
        mime_map = {
            ".pdf": "application/pdf",
            ".py":  "text/x-python",
            ".c":   "text/x-csrc",
            ".cpp": "text/x-c++src",
            ".txt": "text/plain",
            ".md":  "text/markdown",
            ".jpg": "image/jpeg",
            ".png": "image/png",
            ".mp4": "video/mp4",
            ".mp3": "audio/mpeg",
            ".zip": "application/zip",
            ".json":"application/json",
            ".csv": "text/csv",
        }
        return mime_map.get(ext, "application/octet-stream")

    def _extract_text(self, path: str, mime: str) -> str:
        """Extract text content for embedding. Text files read directly."""
        try:
            if "text" in mime:
                with open(path, "r", errors="replace", encoding="utf-8") as f:
                    return f.read(8192)
            elif mime == "application/pdf":
                # Real: use pypdf or pdfminer
                return f"[PDF: {os.path.basename(path)}]"
            elif mime == "application/json":
                with open(path, "r") as f:
                    return f.read(4096)
            else:
                # Binary: just use filename as text
                return os.path.basename(path)
        except Exception:
            return ""

    def _parse_time_filter(self, query: str) -> Optional[float]:
        """Parse time expressions like 'last week', 'today', 'yesterday'."""
        now = time.time()
        q   = query.lower()
        if "today"     in q: return now - 86400
        if "yesterday" in q: return now - 172800
        if "last week" in q: return now - 7 * 86400
        if "last month"in q: return now - 30 * 86400
        return None

    def _background_index_scan(self, root: str):
        """Walk filesystem and index all files in background."""
        print(f"[FS] Background indexing: {root}")
        count = 0
        for dirpath, _, filenames in os.walk(root):
            for fname in filenames:
                fpath = os.path.join(dirpath, fname)
                # Skip already-indexed and unchanged files
                rec = self.index.records.get(fpath)
                try:
                    mtime = os.stat(fpath).st_mtime
                except Exception:
                    continue
                if rec and abs(rec.mtime - mtime) < 1.0:
                    continue
                self.index_file(fpath)
                count += 1
                if count % 100 == 0:
                    self.index.save_to_disk(INDEX_PATH)
                    print(f"[FS] Indexed {count} files...")
        self.index.save_to_disk(INDEX_PATH)
        print(f"[FS] Initial index complete: {count} files")

    def _read_ipc_message(self, ipc) -> Optional[dict]:
        try:
            line = ipc.readline()
            if line:
                return json.loads(line.decode())
        except Exception:
            pass
        return None

    def _send_ipc_response(self, mtype: str, payload: str):
        msg = json.dumps({"type": mtype, "payload": payload}) + "\n"
        try:
            with open(IPC_DEV, "wb") as f:
                f.write(msg.encode())
        except Exception as e:
            print(f"[FS] IPC send error: {e}")


# ===========================================================================
# Entry point (runs as system daemon on boot)
# ===========================================================================
if __name__ == "__main__":
    svc = AIFilesystemService()
    svc.init()
    svc.run()
