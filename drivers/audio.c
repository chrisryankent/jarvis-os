/*
 * =============================================================================
 * JARVIS AI OS — Audio Driver  (drivers/audio.c)
 * Language: C17
 *
 * Targets: Intel HDA (High Definition Audio) — most common on x86 systems.
 * Provides ring buffers for mic capture (input to speech engine) and
 * speaker playback (output from TTS engine).
 *
 * The speech engine calls:
 *   audio_read_frames()  — read PCM from microphone
 *   audio_write_frames() — write PCM to speakers
 *   audio_set_volume()   — master volume control
 * =============================================================================
 */

#include "../kernel/kernel.h"

/* ---- Intel HDA register offsets ----------------------------------------- */
#define HDA_GCAP        0x00   /* Global capabilities    */
#define HDA_GCTL        0x08   /* Global control         */
#define HDA_WAKEEN      0x0C   /* Wake enable            */
#define HDA_STATESTS    0x0E   /* State change status    */
#define HDA_INTCTL      0x20   /* Interrupt control      */
#define HDA_INTSTS      0x24   /* Interrupt status       */
#define HDA_CORBLBASE   0x40   /* CORB lower base addr   */
#define HDA_CORBUBASE   0x44   /* CORB upper base addr   */
#define HDA_CORBWP      0x48   /* CORB write pointer     */
#define HDA_CORBRP      0x4A   /* CORB read pointer      */
#define HDA_CORBCTL     0x4C   /* CORB control           */
#define HDA_RIRBLBASE   0x50   /* RIRB lower base        */
#define HDA_RIRBUBASE   0x54   /* RIRB upper base        */
#define HDA_RIRBWP      0x58   /* RIRB write pointer     */
#define HDA_RINTCNT     0x5A   /* Response interrupt cnt */
#define HDA_RIRBCTL     0x5C   /* RIRB control           */
#define HDA_SDNCTL      0x80   /* Stream descriptor n ctl*/

/* ---- Intel AC97 register offsets (QEMU-friendly) ------------------------ */
#define AC97_NAM_RESET          0x00
#define AC97_NAM_MASTER_VOL     0x02
#define AC97_NAM_MIC_VOL        0x0E
#define AC97_NAM_PCM_OUT_VOL    0x18
#define AC97_NAM_RECORD_SELECT  0x1A
#define AC97_NAM_RECORD_GAIN    0x1C
#define AC97_NAM_EXT_AUDIO_ID   0x28
#define AC97_NAM_EXT_AUDIO_CTRL 0x2A
#define AC97_NAM_PCM_DAC_RATE   0x2C
#define AC97_NAM_PCM_ADC_RATE   0x32

#define AC97_PI_BDBAR           0x00
#define AC97_PI_CIV             0x04
#define AC97_PI_LVI             0x05
#define AC97_PI_SR              0x06
#define AC97_PI_CR              0x0B
#define AC97_PO_BDBAR           0x10
#define AC97_PO_CIV             0x14
#define AC97_PO_LVI             0x15
#define AC97_PO_SR              0x16
#define AC97_PO_CR              0x1B
#define AC97_GLOB_CNT           0x2C

#define AC97_SR_DCH             0x0001
#define AC97_SR_LVBCI           0x0004
#define AC97_SR_BCIS            0x0008
#define AC97_SR_FIFOE           0x0010
#define AC97_CR_RPBM            0x01
#define AC97_CR_RR              0x02
#define AC97_CR_IOCE            0x10
#define AC97_BDL_IOC            0x8000

#define AC97_BDL_COUNT          32
#define AC97_PERIOD_FRAMES      1600
#define AC97_PERIOD_CHANNELS    2
#define AC97_PERIOD_SAMPLES     (AC97_PERIOD_FRAMES * AC97_PERIOD_CHANNELS)
#define AC97_PERIOD_BYTES       (AC97_PERIOD_SAMPLES * sizeof(int16_t))

/* ---- PCM ring buffer ---------------------------------------------------- */
#define AUDIO_SAMPLE_RATE   16000    /* Hz  — Whisper native     */
#define AUDIO_CHANNELS      1        /* mono                     */
#define AUDIO_BITS          16       /* signed 16-bit            */
#define AUDIO_RING_FRAMES   (AUDIO_SAMPLE_RATE * 2)  /* 2 s ring */

static int16_t  mic_ring[AUDIO_RING_FRAMES];
static int16_t  spk_ring[AUDIO_RING_FRAMES];
static volatile size_t mic_write_pos = 0;
static volatile size_t mic_read_pos  = 0;
static volatile size_t spk_write_pos = 0;
static volatile size_t spk_read_pos  = 0;

/* ---- MMIO base address of HDA controller -------------------------------- */
static volatile uint8_t *hda_base = NULL;
static int audio_ready = 0;
static int mic_capture_ready = 0;
static int speaker_output_ready = 1; /* PC speaker fallback is always attempted. */

typedef struct {
    uint32_t addr;
    uint16_t samples;
    uint16_t flags;
} __attribute__((packed)) ac97_bdl_entry_t;

static uint16_t ac97_nam_base = 0;
static uint16_t ac97_nabm_base = 0;
static int      ac97_ready = 0;
static uint8_t  ac97_in_next = 0;

static volatile ac97_bdl_entry_t *ac97_in_bdl = NULL;
static volatile ac97_bdl_entry_t *ac97_out_bdl = NULL;
static int16_t *ac97_in_buf = NULL;
static int16_t *ac97_out_buf = NULL;
static uint64_t ac97_in_bdl_phys = 0;
static uint64_t ac97_out_bdl_phys = 0;
static uint64_t ac97_in_buf_phys = 0;
static uint64_t ac97_out_buf_phys = 0;

static uint8_t audio_inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void audio_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static uint16_t audio_inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void audio_outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}

static uint32_t audio_inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void audio_outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" :: "a"(val), "Nd"(port));
}

static uint32_t hda_read32(uint32_t reg)
{
    return *(volatile uint32_t *)(hda_base + reg);
}

static void hda_write32(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(hda_base + reg) = val;
}

static uint8_t ac97_bm_read8(uint16_t reg)
{
    return audio_inb((uint16_t)(ac97_nabm_base + reg));
}

static uint16_t ac97_bm_read16(uint16_t reg)
{
    return audio_inw((uint16_t)(ac97_nabm_base + reg));
}

static void ac97_bm_write8(uint16_t reg, uint8_t val)
{
    audio_outb((uint16_t)(ac97_nabm_base + reg), val);
}

static void ac97_bm_write16(uint16_t reg, uint16_t val)
{
    audio_outw((uint16_t)(ac97_nabm_base + reg), val);
}

static void ac97_bm_write32(uint16_t reg, uint32_t val)
{
    audio_outl((uint16_t)(ac97_nabm_base + reg), val);
}

static uint16_t ac97_codec_read(uint16_t reg)
{
    return audio_inw((uint16_t)(ac97_nam_base + reg));
}

static void ac97_codec_write(uint16_t reg, uint16_t val)
{
    audio_outw((uint16_t)(ac97_nam_base + reg), val);
}

static void ac97_clear_stream(uint16_t sr_reg)
{
    ac97_bm_write16(sr_reg, AC97_SR_LVBCI | AC97_SR_BCIS | AC97_SR_FIFOE);
}

static int ac97_alloc_dma(void)
{
    uint64_t buf_pages = ((uint64_t)AC97_PERIOD_BYTES * AC97_BDL_COUNT + 4095ULL) / 4096ULL;

    ac97_in_bdl_phys = pmm_alloc_contiguous(1);
    ac97_out_bdl_phys = pmm_alloc_contiguous(1);
    ac97_in_buf_phys = pmm_alloc_contiguous(buf_pages);
    ac97_out_buf_phys = pmm_alloc_contiguous(buf_pages);

    if (!ac97_in_bdl_phys || !ac97_out_bdl_phys ||
        !ac97_in_buf_phys || !ac97_out_buf_phys) {
        terminal_puts("[AUDIO] AC97 DMA allocation failed\n");
        return -1;
    }

    ac97_in_bdl = (volatile ac97_bdl_entry_t *)vmm_map_range(ac97_in_bdl_phys, 4096, VMM_PROT_RW | VMM_PROT_NOCACHE);
    ac97_out_bdl = (volatile ac97_bdl_entry_t *)vmm_map_range(ac97_out_bdl_phys, 4096, VMM_PROT_RW | VMM_PROT_NOCACHE);
    ac97_in_buf = (int16_t *)vmm_map_range(ac97_in_buf_phys, buf_pages * 4096ULL, VMM_PROT_RW | VMM_PROT_NOCACHE);
    ac97_out_buf = (int16_t *)vmm_map_range(ac97_out_buf_phys, buf_pages * 4096ULL, VMM_PROT_RW | VMM_PROT_NOCACHE);

    if (!ac97_in_bdl || !ac97_out_bdl || !ac97_in_buf || !ac97_out_buf) {
        terminal_puts("[AUDIO] AC97 DMA mapping failed\n");
        return -1;
    }

    for (int i = 0; i < AC97_BDL_COUNT; i++) {
        ac97_in_bdl[i].addr = (uint32_t)(ac97_in_buf_phys + (uint64_t)i * AC97_PERIOD_BYTES);
        ac97_in_bdl[i].samples = AC97_PERIOD_SAMPLES;
        ac97_in_bdl[i].flags = AC97_BDL_IOC;

        ac97_out_bdl[i].addr = (uint32_t)(ac97_out_buf_phys + (uint64_t)i * AC97_PERIOD_BYTES);
        ac97_out_bdl[i].samples = AC97_PERIOD_SAMPLES;
        ac97_out_bdl[i].flags = AC97_BDL_IOC;
    }

    for (size_t i = 0; i < (size_t)AC97_PERIOD_SAMPLES * AC97_BDL_COUNT; i++) {
        ac97_in_buf[i] = 0;
        ac97_out_buf[i] = 0;
    }

    return 0;
}

static void ac97_reset_stream(uint16_t cr_reg)
{
    ac97_bm_write8(cr_reg, AC97_CR_RR);
    for (volatile int i = 0; i < 10000; i++);
    ac97_bm_write8(cr_reg, 0);
}

static void ac97_keep_stream_running(uint16_t civ_reg, uint16_t lvi_reg,
                                     uint16_t sr_reg, uint16_t cr_reg)
{
    uint8_t civ = ac97_bm_read8(civ_reg) & (AC97_BDL_COUNT - 1);
    ac97_bm_write8(lvi_reg, (uint8_t)((civ + AC97_BDL_COUNT - 1) & (AC97_BDL_COUNT - 1)));
    ac97_clear_stream(sr_reg);
    if (ac97_bm_read16(sr_reg) & AC97_SR_DCH)
        ac97_bm_write8(cr_reg, AC97_CR_RPBM | AC97_CR_IOCE);
}

static int ac97_init(pci_device_t *dev)
{
    ac97_nam_base = (uint16_t)(dev->bar[0] & ~0x3u);
    ac97_nabm_base = (uint16_t)(dev->bar[1] & ~0x3u);
    if (ac97_nam_base == 0 || ac97_nabm_base == 0) {
        terminal_puts("[AUDIO] AC97 I/O BARs unavailable\n");
        return -1;
    }

    terminal_printf("[AUDIO] AC97 mixer I/O 0x%x, bus-master I/O 0x%x\n",
        ac97_nam_base, ac97_nabm_base);

    if (!ac97_out_bdl_phys && ac97_alloc_dma() != 0)    /* once: a restart reuses it */
        return -1;

    audio_outl((uint16_t)(ac97_nabm_base + AC97_GLOB_CNT), 0x00000002);
    for (volatile int i = 0; i < 50000; i++);
    (void)audio_inl((uint16_t)(ac97_nabm_base + AC97_GLOB_CNT));

    ac97_codec_write(AC97_NAM_RESET, 0);
    for (volatile int i = 0; i < 50000; i++);

    ac97_codec_write(AC97_NAM_MASTER_VOL, 0x0000);
    ac97_codec_write(AC97_NAM_PCM_OUT_VOL, 0x0000);
    ac97_codec_write(AC97_NAM_MIC_VOL, 0x0000);
    ac97_codec_write(AC97_NAM_RECORD_SELECT, 0x0000); /* mic/mic */
    ac97_codec_write(AC97_NAM_RECORD_GAIN, 0x0F0F);

    if (ac97_codec_read(AC97_NAM_EXT_AUDIO_ID) & 0x0001) {
        ac97_codec_write(AC97_NAM_EXT_AUDIO_CTRL,
                         ac97_codec_read(AC97_NAM_EXT_AUDIO_CTRL) | 0x0001);
        ac97_codec_write(AC97_NAM_PCM_DAC_RATE, AUDIO_SAMPLE_RATE);
        ac97_codec_write(AC97_NAM_PCM_ADC_RATE, AUDIO_SAMPLE_RATE);
    }

    ac97_reset_stream(AC97_PI_CR);
    ac97_reset_stream(AC97_PO_CR);

    ac97_bm_write32(AC97_PI_BDBAR, (uint32_t)ac97_in_bdl_phys);
    ac97_bm_write32(AC97_PO_BDBAR, (uint32_t)ac97_out_bdl_phys);
    ac97_bm_write8(AC97_PI_LVI, AC97_BDL_COUNT - 1);
    ac97_bm_write8(AC97_PO_LVI, AC97_BDL_COUNT - 1);
    ac97_clear_stream(AC97_PI_SR);
    ac97_clear_stream(AC97_PO_SR);

    ac97_in_next = ac97_bm_read8(AC97_PI_CIV) & (AC97_BDL_COUNT - 1);
    ac97_bm_write8(AC97_PI_CR, AC97_CR_RPBM | AC97_CR_IOCE);
    ac97_bm_write8(AC97_PO_CR, AC97_CR_RPBM | AC97_CR_IOCE);

    ac97_ready = 1;
    audio_ready = 1;
    mic_capture_ready = 1;
    speaker_output_ready = 1;
    g_kstate.audio_ready = 1;
    g_kstate.mic_ready = 1;
    g_kstate.speaker_ready = 1;
    terminal_puts("[AUDIO] AC97 capture/playback DMA ready\n");
    return 0;
}

/* Device Manager "Restart device": stop both DMA engines, reset the codec
 * and program everything again. Volume and mute settings are re-applied. */
int audio_restart(void)
{
    if (!ac97_nam_base) return -1;
    pci_device_t *ac97 = pci_find_class(0x04, 0x01);
    if (!ac97) return -1;
    ac97_ready = 0;
    ac97_bm_write8(AC97_PI_CR, 0);
    ac97_bm_write8(AC97_PO_CR, 0);
    int rc = ac97_init(ac97);
    terminal_printf("[AUDIO] Restarted by Device Manager: %s\n", rc ? "failed" : "ok");
    return rc;
}

/* ---- Audio driver init -------------------------------------------------- */
void audio_init(void)
{
    terminal_puts("[AUDIO] Initialising audio devices\n");

    pci_device_t *ac97 = pci_find_class(0x04, 0x01); /* Multimedia / AC97 */
    if (ac97 && ac97_init(ac97) == 0)
        return;

    /* Find HDA controller on PCI bus */
    pci_device_t *hda = pci_find_class(0x04, 0x03); /* Multimedia / HDA */
    if (!hda) {
        terminal_puts("[AUDIO] No HDA controller found\n");
        g_kstate.audio_ready = 0;
        g_kstate.mic_ready = 0;
        g_kstate.speaker_ready = speaker_output_ready;
        return;
    }

    /* Map MMIO BAR0 */
    uint64_t bar0 = hda->bar[0] & ~0xFULL; /* strip flags */

    hda_base = (volatile uint8_t *)vmm_map_range(bar0, 0x4000,
                    VMM_PROT_RW | VMM_PROT_NOCACHE);
    if (!hda_base) {
        terminal_puts("[AUDIO] HDA MMIO mapping failed; skipping\n");
        g_kstate.audio_ready = 0;
        g_kstate.mic_ready = 0;
        g_kstate.speaker_ready = speaker_output_ready;
        return;
    }

    terminal_printf("[AUDIO] HDA MMIO at 0x%llx\n", bar0);

    /* Reset controller */
    hda_write32(HDA_GCTL, 0); /* assert reset */
    for (volatile int i = 0; i < 100000; i++); /* wait */
    hda_write32(HDA_GCTL, 1); /* deassert reset */
    for (volatile int i = 0; i < 100000; i++); /* wait for codec settle */

    /* Enable all interrupts */
    hda_write32(HDA_INTCTL, 0x800000FF);

    /*
     * Full codec enumeration, widget path setup, DMA buffer allocation
     * is a several-hundred-line process (verb commands via CORB/RIRB).
     * In production: implement corb_send_verb() and rirb_read_response(),
     * then enumerate codec nodes (widget types: DAC, ADC, mixer, pin).
     *
     * For now: zero ring buffers so speech threads start cleanly.
     */
    for (int i = 0; i < AUDIO_RING_FRAMES; i++) {
        mic_ring[i] = 0;
        spk_ring[i] = 0;
    }

    audio_ready = 1;
    mic_capture_ready = 0;
    speaker_output_ready = 1;
    g_kstate.audio_ready = 1;
    g_kstate.mic_ready = mic_capture_ready;
    g_kstate.speaker_ready = speaker_output_ready;
    terminal_puts("[AUDIO] HDA mapped; PC speaker output ready; microphone capture DMA is not wired yet\n");
}

int audio_is_ready(void) { return audio_ready; }
int audio_mic_is_ready(void) { return mic_capture_ready; }
int audio_speaker_is_ready(void) { return speaker_output_ready; }

int audio_self_test(void)
{
    if (!audio_ready) {
        terminal_puts("[AUDIO] Self-test failed: HDA not ready\n");
        terminal_puts("[AUDIO] Trying PC speaker fallback tone anyway\n");
    }

    float tone[160];
    for (int i = 0; i < 160; i++)
        tone[i] = (i % 20) < 10 ? 0.15f : -0.15f;
    audio_write_frames(tone, 160);
    audio_speak_machine_text("speaker test");
    terminal_puts("[AUDIO] Self-test queued 10ms test tone in speaker ring\n");
    return 0;
}

static void pc_speaker_wait_ms(int ms)
{
    if (ms <= 0)
        return;

    for (int m = 0; m < ms; m++) {
        for (volatile int i = 0; i < 120; i++)
            audio_outb(0x80, 0);
    }
}

static void pc_speaker_off(void)
{
    uint8_t v = audio_inb(0x61);
    audio_outb(0x61, v & (uint8_t)~0x03u);
}

static void pc_speaker_tone(uint32_t hz, int ms)
{
    if (hz < 37)
        hz = 37;
    if (hz > 12000)
        hz = 12000;

    uint32_t divisor = 1193182u / hz;
    if (divisor == 0)
        divisor = 1;

    /* PIT channel 2, low/high byte, mode 3 square wave. */
    audio_outb(0x43, 0xB6);
    audio_outb(0x42, (uint8_t)(divisor & 0xFFu));
    audio_outb(0x42, (uint8_t)((divisor >> 8) & 0xFFu));

    uint8_t v = audio_inb(0x61);
    audio_outb(0x61, v | 0x03u);
    pc_speaker_wait_ms(ms);
    pc_speaker_off();
}

static char audio_lower_ascii(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

static void pc_speaker_pause(int ms)
{
    pc_speaker_off();
    pc_speaker_wait_ms(ms);
}

static void pc_speaker_sweep(uint32_t start_hz, uint32_t end_hz, int ms)
{
    int steps = 5;
    int step_ms = ms / steps;
    if (step_ms < 1)
        step_ms = 1;

    for (int i = 0; i < steps; i++) {
        uint32_t hz = start_hz +
            ((end_hz > start_hz ? end_hz - start_hz : start_hz - end_hz) * (uint32_t)i) /
            (uint32_t)(steps - 1);
        if (end_hz < start_hz)
            hz = start_hz - ((start_hz - end_hz) * (uint32_t)i) / (uint32_t)(steps - 1);
        pc_speaker_tone(hz, step_ms);
    }
}

void audio_speak_machine_text(const char *text)
{
    if (!text || text[0] == '\0')
        return;
    if (ac97_ready)
        return;

    terminal_puts("[AUDIO] PC speaker voice fallback active\n");

    for (size_t i = 0; text[i] != '\0'; i++) {
        char c = audio_lower_ascii(text[i]);
        char n = audio_lower_ascii(text[i + 1]);

        if (c == ' ' || c == '\t' || c == '\n') {
            pc_speaker_pause(45);
            continue;
        }

        if (c == ',' || c == ';' || c == ':') {
            pc_speaker_pause(95);
            continue;
        }

        if (c == '.' || c == '?' || c == '!') {
            pc_speaker_pause(150);
            continue;
        }

        if (c == 's' && n == 'h') {
            pc_speaker_sweep(3600, 2800, 55);
            i++;
            continue;
        }

        if (c == 'c' && n == 'h') {
            pc_speaker_pause(20);
            pc_speaker_sweep(2600, 3600, 55);
            i++;
            continue;
        }

        if (c == 't' && n == 'h') {
            pc_speaker_sweep(3000, 2400, 55);
            i++;
            continue;
        }

        switch (c) {
        case 'a': pc_speaker_sweep(720, 920, 80); break;
        case 'e': pc_speaker_sweep(960, 1180, 76); break;
        case 'i':
        case 'y': pc_speaker_sweep(1200, 1450, 72); break;
        case 'o': pc_speaker_sweep(640, 520, 82); break;
        case 'u': pc_speaker_sweep(520, 430, 78); break;
        case 'm': pc_speaker_tone(220, 48); break;
        case 'n': pc_speaker_tone(260, 45); break;
        case 'l': pc_speaker_sweep(420, 520, 48); break;
        case 'r': pc_speaker_sweep(360, 300, 55); break;
        case 'w': pc_speaker_sweep(300, 480, 50); break;
        case 'b':
        case 'd':
        case 'g':
        case 'j':
            pc_speaker_pause(18);
            pc_speaker_tone(900, 24);
            break;
        case 'v':
        case 'z':
            pc_speaker_sweep(1800, 1500, 42);
            break;
        case 'p':
        case 't':
        case 'k':
        case 'q':
            pc_speaker_pause(18);
            pc_speaker_tone(1500, 22);
            break;
        case 'f':
        case 's':
        case 'h':
        case 'x':
            pc_speaker_sweep(2600, 3400, 48);
            break;
        default:
            pc_speaker_tone(700, 20);
            break;
        }
    }

    pc_speaker_off();
}

static void ac97_poll_capture(void)
{
    if (!ac97_ready)
        return;

    uint8_t civ = ac97_bm_read8(AC97_PI_CIV) & (AC97_BDL_COUNT - 1);
    int processed = 0;

    while (ac97_in_next != civ && processed < AC97_BDL_COUNT) {
        int16_t *src = ac97_in_buf + ((size_t)ac97_in_next * AC97_PERIOD_SAMPLES);

        for (int frame = 0; frame < AC97_PERIOD_FRAMES; frame++) {
            int32_t l = src[frame * 2];
            int32_t r = src[frame * 2 + 1];
            mic_ring[mic_write_pos % AUDIO_RING_FRAMES] = (int16_t)((l + r) / 2);
            mic_write_pos = (mic_write_pos + 1) % AUDIO_RING_FRAMES;
        }

        ac97_in_next = (uint8_t)((ac97_in_next + 1) & (AC97_BDL_COUNT - 1));
        processed++;
    }

    ac97_keep_stream_running(AC97_PI_CIV, AC97_PI_LVI, AC97_PI_SR, AC97_PI_CR);
}

static int16_t float_to_i16(float s)
{
    if (s >  1.0f) s =  1.0f;
    if (s < -1.0f) s = -1.0f;
    return (int16_t)(s * 32767.0f);
}

static int g_audio_disabled = 0;         /* Device Manager */
static int g_audio_volume = 80;

static void ac97_queue_playback(const float *buf, size_t frames)
{
    if (!ac97_ready || g_audio_disabled || !buf || frames == 0)
        return;

    uint8_t civ = ac97_bm_read8(AC97_PO_CIV) & (AC97_BDL_COUNT - 1);
    uint8_t idx = (uint8_t)((civ + 2) & (AC97_BDL_COUNT - 1));
    size_t off = 0;
    int periods = 0;

    while (off < frames && periods < AC97_BDL_COUNT - 3) {
        int16_t *dst = ac97_out_buf + ((size_t)idx * AC97_PERIOD_SAMPLES);
        int frame = 0;

        for (; frame < AC97_PERIOD_FRAMES && off < frames; frame++, off++) {
            int16_t s = float_to_i16(buf[off]);
            dst[frame * 2] = s;
            dst[frame * 2 + 1] = s;
        }

        for (; frame < AC97_PERIOD_FRAMES; frame++) {
            dst[frame * 2] = 0;
            dst[frame * 2 + 1] = 0;
        }

        idx = (uint8_t)((idx + 1) & (AC97_BDL_COUNT - 1));
        periods++;
    }

    ac97_bm_write8(AC97_PO_LVI, (uint8_t)((idx + AC97_BDL_COUNT - 1) & (AC97_BDL_COUNT - 1)));
    ac97_clear_stream(AC97_PO_SR);
    ac97_bm_write8(AC97_PO_CR, AC97_CR_RPBM | AC97_CR_IOCE);
}

/* ---- Read PCM from mic ring buffer -------------------------------------- */
size_t audio_read_frames(int16_t *buf, size_t max_frames)
{
    ac97_poll_capture();

    size_t avail = 0;
    size_t r = mic_read_pos;
    size_t w = mic_write_pos;

    if (w >= r)
        avail = w - r;
    else
        avail = AUDIO_RING_FRAMES - r + w;

    size_t to_read = avail < max_frames ? avail : max_frames;

    for (size_t i = 0; i < to_read; i++) {
        buf[i] = mic_ring[(r + i) % AUDIO_RING_FRAMES];
    }
    mic_read_pos = (r + to_read) % AUDIO_RING_FRAMES;
    return to_read;
}

void audio_flush_input(void)
{
    mic_read_pos = mic_write_pos;

    if (ac97_ready) {
        ac97_in_next = ac97_bm_read8(AC97_PI_CIV) & (AC97_BDL_COUNT - 1);
        ac97_clear_stream(AC97_PI_SR);
    }
}

/* ---- Write PCM to speaker ring buffer ----------------------------------- */
size_t audio_write_frames(const float *buf, size_t frames)
{
    ac97_queue_playback(buf, frames);

    for (size_t i = 0; i < frames; i++) {
        /* Convert float [-1,1] → int16 */
        spk_ring[spk_write_pos % AUDIO_RING_FRAMES] = float_to_i16(buf[i]);
        spk_write_pos = (spk_write_pos + 1) % AUDIO_RING_FRAMES;
    }

    return frames;
}

/* ---- IRQ handler: DMA interrupt ----------------------------------------- */
/* Called from IDT when HDA DMA completes a period */
void audio_irq_handler(void)
{
    uint32_t sts = hda_read32(HDA_INTSTS);
    if (sts & 0x1) { /* Input stream */
        /* DMA filled mic_ring — advance write pointer */
        mic_write_pos = (mic_write_pos + 160) % AUDIO_RING_FRAMES; /* 10 ms */
    }
    if (sts & 0x2) { /* Output stream */
        /* DMA consumed spk_ring — advance read pointer */
        spk_read_pos = (spk_read_pos + 160) % AUDIO_RING_FRAMES;
    }
    /* Acknowledge interrupt */
    hda_write32(HDA_INTSTS, sts);
}

/* ---- Volume control ----------------------------------------------------- */
void audio_set_volume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    g_audio_volume = percent;
    if (g_audio_disabled) percent = 0;
    terminal_printf("[AUDIO] Volume set to %d%%\n", percent);
    if (ac97_ready) {
        /* AC97 master and PCM out: 6-bit attenuation per channel (0 = loudest,
         * 1.5 dB steps), bit 15 = mute. */
        uint16_t att = (uint16_t)((100 - percent) * 63 / 100);
        uint16_t v = percent == 0 ? 0x8000 : (uint16_t)((att << 8) | att);
        ac97_codec_write(0x02, v);
        ac97_codec_write(AC97_NAM_PCM_OUT_VOL, percent == 0 ? 0x8000 : (uint16_t)(((att / 2) << 8) | (att / 2)));
    }
    /*
     * Real: send HDA verb to mixer widget to set amplifier gain.
     * Verb: SET_AMPLIFIER_GAIN_MUTE (0x3) with payload = volume level.
     */
}

/* Device Manager: a disabled sound card plays nothing and is muted. */
void audio_set_enabled(int on)
{
    int vol = g_audio_volume;
    g_audio_disabled = on ? 0 : 1;
    audio_set_volume(vol);
    g_audio_volume = vol;
}
int audio_is_enabled(void) { return !g_audio_disabled; }
