/*
 * DisplayPort for the Radeon driver, as Linux's radeon driver does it
 * (atombios_dp.c, with drm_dp_helper.c's AUX and training rules). The
 * Llano APUs have no LVDS or VGA output of their own: a laptop's panel
 * hangs off a DisplayPort-to-LVDS bridge (TRAVIS), a VGA port off a
 * DisplayPort-to-VGA one (NUTMEG), so even an LVDS panel is driven as a
 * DisplayPort sink.
 *
 * The AUX channel, the sideband the sink's registers (its DPCD) and its
 * I2C bus are reached over, is run by the video BIOS's
 * ProcessAuxChannelTransaction table: the request goes into the
 * interpreter's scratch area, the reply comes back there. Each
 * transaction carries at most 16 bytes (12 for a write).
 *
 * Link training brings the main link up once the transmitter is on:
 * clock recovery with training pattern 1, raising the voltage swing and
 * pre-emphasis the sink asks for, then channel equalisation with pattern
 * 2. The DIG encoder sends the patterns (radeon_dp_encoder in
 * radeon_llano_modeset.c), the transmitter sets the drive levels.
 */
#include "kernel.h"
#include "drivers.h"
#include "radeon.h"

#define PACKED __attribute__((packed))

struct dp_sink dpsink;

/* AUX requests (in the top nibble of the header's third byte) */
#define AUX_I2C_WRITE    0x0
#define AUX_I2C_READ     0x1
#define AUX_I2C_MOT      0x4        /* middle of transaction: no I2C stop after it */
#define AUX_NATIVE_WRITE 0x8
#define AUX_NATIVE_READ  0x9

/* Replies: the native one in bits 1:0, the I2C one in bits 3:2 */
#define AUX_NATIVE_REPLY_MASK  0x3
#define AUX_NATIVE_REPLY_ACK   0x0
#define AUX_NATIVE_REPLY_DEFER 0x2
#define AUX_I2C_REPLY_MASK     0xC
#define AUX_I2C_REPLY_ACK      0x0
#define AUX_I2C_REPLY_DEFER    0x8

#define EDID_I2C_ADDR 0x50

/* DPCD registers */
#define DP_DPCD_REV               0x000
#define DP_MAX_LINK_RATE          0x001
#define DP_MAX_LANE_COUNT         0x002
#define DP_MAX_LANE_COUNT_MASK      0x1F
#define DP_ENHANCED_FRAME_CAP       0x80
#define DP_MAX_DOWNSPREAD         0x003
#define DP_TRAINING_AUX_RD_INTERVAL 0x00E
#define DP_EDP_CONFIGURATION_CAP  0x00D
#define DP_LINK_BW_SET            0x100
#define DP_LANE_COUNT_SET         0x101
#define DP_LANE_COUNT_ENHANCED_FRAME_EN 0x80
#define DP_TRAINING_PATTERN_SET   0x102
#define DP_TRAINING_PATTERN_DISABLE 0
#define DP_TRAINING_PATTERN_1       1
#define DP_TRAINING_PATTERN_2       2
#define DP_TRAINING_LANE0_SET     0x103
#define DP_DOWNSPREAD_CTRL        0x107
#define DP_SPREAD_AMP_0_5           0x10
#define DP_EDP_CONFIGURATION_SET  0x10A
#define DP_LANE0_1_STATUS         0x202
#define DP_LINK_STATUS_SIZE         6
#define DP_SET_POWER              0x600
#define DP_SET_POWER_D0             1
#define DP_SET_POWER_D3             2

/* Link status: per lane, a nibble of these */
#define DP_LANE_CR_DONE        0x1
#define DP_LANE_CHANNEL_EQ_DONE 0x2
#define DP_LANE_SYMBOL_LOCKED  0x4
#define DP_INTERLANE_ALIGN_DONE 0x1  /* in the third status byte */

/* A lane's drive levels, as the training set bytes carry them */
#define DP_TRAIN_VOLTAGE_SWING_MASK 0x03
#define DP_TRAIN_MAX_SWING_REACHED  0x04
#define DP_TRAIN_PRE_EMPHASIS_MASK  0x18
#define DP_TRAIN_PRE_EMPHASIS_SHIFT 3
#define DP_TRAIN_MAX_PRE_EMPHASIS_REACHED 0x20
#define DP_VOLTAGE_MAX       3
#define DP_PRE_EMPHASIS_MAX  (3 << DP_TRAIN_PRE_EMPHASIS_SHIFT)

/* ProcessAuxChannelTransaction 1.2: offsets into the scratch area */
struct aux_args {
    uint16_t request, data_out;
    uint8_t channel;
    uint8_t delay_reply;            /* in: delay in 10 us; out: the reply status */
    uint8_t out_len;
    uint8_t hpd;
} PACKED;

#define AUX_REQUEST_OFF 4
#define AUX_REPLY_OFF   20

/* One transaction. Returns the bytes moved, with the sink's reply in
 * *reply, or -ETIMEDOUT / -EIO when the channel itself failed. */
static int aux_xfer(int request, uint32_t addr, const uint8_t *tx, uint8_t *rx, int n, uint8_t *reply)
{
    int read = (request & 3) == 1;
    uint8_t *scratch = radeon_atom_scratch();
    if (!scratch || n > 16 || (!read && n > 12))
        return -EIO;
    uint8_t *req = scratch + AUX_REQUEST_OFF;
    int size = 4;
    req[0] = (uint8_t)addr;
    req[1] = (uint8_t)(addr >> 8);
    req[2] = (uint8_t)(request << 4 | ((addr >> 16) & 0xF));
    req[3] = (uint8_t)(n ? n - 1 : 0);
    if (!read && n) {
        memcpy(req + 4, tx, (size_t)n);
        size += n;
    }
    /* The table wants the request's length here too; 3 for a bare address. */
    req[3] |= (uint8_t)((n ? size : 3) << 4);

    struct aux_args a;
    memset(&a, 0, sizeof(a));
    a.request = AUX_REQUEST_OFF;
    a.data_out = AUX_REPLY_OFF;
    a.channel = dpsink.channel;
    a.hpd = dpsink.hpd;
    if (radeon_atom_run(ATOM_CMD_PROCESS_AUX, &a, sizeof(a)) < 0)
        return -EIO;
    switch (a.delay_reply) {
    case 1:
        return -ETIMEDOUT;
    case 2:                         /* flags not zero */
    case 3:                         /* error */
        return -EIO;
    }
    *reply = a.delay_reply >> 4;
    if (!read)
        return n;
    int got = MIN((int)a.out_len, n);
    if (rx && got > 0)
        memcpy(rx, scratch + AUX_REPLY_OFF, (size_t)got);
    return got;
}

/* A native read or write of up to 16 bytes, retried while the sink defers. */
static int dpcd_access(int request, uint32_t addr, uint8_t *buf, int n)
{
    for (int retry = 0; retry < 32; retry++) {
        uint8_t reply = 0;
        int r = aux_xfer(request, addr, buf, buf, n, &reply);
        if (r >= 0) {
            int native = reply & AUX_NATIVE_REPLY_MASK;
            if (native == AUX_NATIVE_REPLY_ACK && r == n)
                return n;
            if (native != AUX_NATIVE_REPLY_ACK && native != AUX_NATIVE_REPLY_DEFER)
                return -EIO;        /* NACK */
            /* deferred, or a short reply: again */
        } else if (r != -ETIMEDOUT) {
            return r;
        }
        udelay(500);
    }
    return -ETIMEDOUT;
}

int radeon_dp_read(uint32_t addr, void *buf, int n)
{
    uint8_t *p = buf;
    for (int done = 0; done < n;) {
        int chunk = MIN(n - done, 16);
        if (dpcd_access(AUX_NATIVE_READ, addr + (uint32_t)done, p + done, chunk) < 0)
            return -EIO;
        done += chunk;
    }
    return n;
}

/* The request's length (header and payload) has 4 bits: 11 bytes at a time. */
int radeon_dp_write(uint32_t addr, const void *buf, int n)
{
    uint8_t tmp[11];
    const uint8_t *p = buf;
    for (int done = 0; done < n;) {
        int chunk = MIN(n - done, 11);
        memcpy(tmp, p + done, (size_t)chunk);
        if (dpcd_access(AUX_NATIVE_WRITE, addr + (uint32_t)done, tmp, chunk) < 0)
            return -EIO;
        done += chunk;
    }
    return n;
}

static int dpcd_writeb(uint32_t addr, uint8_t v)
{
    return radeon_dp_write(addr, &v, 1);
}

int radeon_dp_read_dpcd(void)
{
    dpsink.dpcd_ok = radeon_dp_read(DP_DPCD_REV, dpsink.dpcd, sizeof(dpsink.dpcd)) > 0;
    if (!dpsink.dpcd_ok)
        dpsink.dpcd[0] = 0;
    return dpsink.dpcd_ok ? 0 : -1;
}

/* ---- I2C over AUX: the sink's EDID ---- */

/* One I2C-over-AUX message to the EDID EEPROM; the bytes moved, or < 0. */
static int i2c_msg(int request, uint8_t *buf, int n)
{
    for (int retry = 0; retry < 32; retry++) {
        uint8_t reply = 0;
        int r = aux_xfer(request, EDID_I2C_ADDR, buf, buf, n, &reply);
        if (r == -ETIMEDOUT || (r >= 0 && ((reply & AUX_NATIVE_REPLY_MASK) == AUX_NATIVE_REPLY_DEFER ||
                                           (reply & AUX_I2C_REPLY_MASK) == AUX_I2C_REPLY_DEFER))) {
            udelay(500);
            continue;
        }
        if (r < 0)
            return r;
        if ((reply & AUX_NATIVE_REPLY_MASK) != AUX_NATIVE_REPLY_ACK ||
            (reply & AUX_I2C_REPLY_MASK) != AUX_I2C_REPLY_ACK)
            return -EIO;
        return r;
    }
    return -ETIMEDOUT;
}

/* All n bytes, in transactions of at most 16; a short reply goes on from there. */
static int i2c_drain(int request, uint8_t *buf, int n)
{
    int chunk = 16;
    for (int done = 0; done < n;) {
        int r = i2c_msg(request, buf + done, MIN(chunk, n - done));
        if (r <= 0)
            return r < 0 ? r : -EIO;
        done += r;
        chunk = r;
    }
    return n;
}

/* EEPROM offset 0, then 128 bytes, the way drm_dp_i2c_xfer sends them:
 * each message opens with a bare address, a bare address without MOT
 * stops the bus at the end. */
int radeon_dp_read_edid(uint8_t *edid)
{
    static const uint8_t header[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    uint8_t offset = 0;
    int ok = i2c_msg(AUX_I2C_WRITE | AUX_I2C_MOT, NULL, 0) >= 0 &&
             i2c_drain(AUX_I2C_WRITE | AUX_I2C_MOT, &offset, 1) >= 0 &&
             i2c_msg(AUX_I2C_READ | AUX_I2C_MOT, NULL, 0) >= 0 &&
             i2c_drain(AUX_I2C_READ | AUX_I2C_MOT, edid, 128) >= 0;
    i2c_msg(AUX_I2C_READ, NULL, 0);
    if (!ok)
        return 0;
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++)
        sum = (uint8_t)(sum + edid[i]);
    return sum == 0 && !memcmp(edid, header, sizeof(header));
}

/* ---- the link ---- */

/* The fewest lanes at the lowest rate that carry the pixel clock at bpp
 * bits per pixel (radeon_dp_get_dp_link_config). NUTMEG runs at 2.7 GHz. */
int radeon_dp_link_config(uint32_t pixclock, int bpp, int nutmeg)
{
    static const uint32_t rates[3] = { 162000, 270000, 540000 };
    uint32_t max_rate = (uint32_t)dpsink.dpcd[DP_MAX_LINK_RATE] * 27000;
    int max_lanes = dpsink.dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK;
    for (int i = 0; i < 3 && rates[i] <= max_rate; i++) {
        uint32_t rate = nutmeg ? 270000 : rates[i];
        for (int lanes = 1; lanes <= max_lanes; lanes <<= 1) {
            if ((uint32_t)lanes * rate * 8 / (uint32_t)bpp >= pixclock) {
                dpsink.lanes = lanes;
                dpsink.link_clock = rate;
                return 0;
            }
        }
        if (nutmeg)
            break;
    }
    dpsink.lanes = 0;
    dpsink.link_clock = 0;
    return -1;
}

/* How the DIG encoder should address the sink (radeon_dp_get_panel_mode):
 * a bridge chip in the internal mode its DPCD offers. From the DPCD read
 * at boot, not a fresh read, which fails while the bridge restarts and
 * would leave it addressed as an external sink: a black panel. */
int radeon_dp_panel_mode(int bridge)
{
    if (!bridge)
        return DP_PANEL_MODE_EXTERNAL_DP_MODE;
    if (dpsink.dpcd[DP_EDP_CONFIGURATION_CAP] & 1)
        return DP_PANEL_MODE_INTERNAL_DP2_MODE;
    return DP_PANEL_MODE_INTERNAL_DP1_MODE;     /* both NUTMEG and TRAVIS */
}

/* Wait, up to a second, until the sink answers on AUX with the DPCD it
 * had at boot (a bridge restarts when it is initialised). Returns the
 * milliseconds it took, or -1. */
int radeon_dp_wait_sink(void)
{
    uint8_t buf[sizeof(dpsink.dpcd)];
    uint32_t start = ticks;
    for (;;) {
        if (radeon_dp_read(DP_DPCD_REV, buf, sizeof(buf)) > 0 &&
            (!dpsink.dpcd_ok || !memcmp(buf, dpsink.dpcd, sizeof(buf))))
            return (int)((ticks - start) * 1000 / HZ);
        if ((int32_t)(ticks - start) >= HZ)
            return -1;
        mdelay(10);
    }
}

void radeon_dp_set_power(int on)
{
    if (dpsink.dpcd[DP_DPCD_REV] >= 0x11) {
        dpcd_writeb(DP_SET_POWER, on ? DP_SET_POWER_D0 : DP_SET_POWER_D3);
        udelay(1000);
    }
}

/* ---- link training (radeon_dp_link_train) ---- */

static struct {
    uint8_t train_set[4];
    uint8_t status[DP_LINK_STATUS_SIZE];
} lt;

static uint8_t lane_status(int lane)
{
    return (uint8_t)(lt.status[lane >> 1] >> ((lane & 1) * 4)) & 0xF;
}

static int clock_recovery_ok(void)
{
    for (int lane = 0; lane < dpsink.lanes; lane++)
        if (!(lane_status(lane) & DP_LANE_CR_DONE))
            return 0;
    return 1;
}

static int channel_eq_ok(void)
{
    if (!(lt.status[2] & DP_INTERLANE_ALIGN_DONE))
        return 0;
    for (int lane = 0; lane < dpsink.lanes; lane++) {
        uint8_t s = lane_status(lane);
        if ((s & (DP_LANE_CR_DONE | DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED)) !=
            (DP_LANE_CR_DONE | DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED))
            return 0;
    }
    return 1;
}

/* The highest swing and pre-emphasis any lane asks for, on every lane. */
static void adjust_train(void)
{
    uint8_t v = 0, p = 0;
    for (int lane = 0; lane < dpsink.lanes; lane++) {
        uint8_t req = (uint8_t)(lt.status[4 + (lane >> 1)] >> ((lane & 1) * 4));
        uint8_t this_v = req & 3, this_p = (uint8_t)((req >> 2 & 3) << DP_TRAIN_PRE_EMPHASIS_SHIFT);
        v = MAX(v, this_v);
        p = MAX(p, this_p);
    }
    if (v >= DP_VOLTAGE_MAX)
        v |= DP_TRAIN_MAX_SWING_REACHED;
    if (p >= DP_PRE_EMPHASIS_MAX)
        p |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
    for (int lane = 0; lane < 4; lane++)
        lt.train_set[lane] = v | p;
}

static void update_vs_emph(void)
{
    radeon_dp_vsemph(lt.train_set[0]);  /* all lanes at once on the source */
    radeon_dp_write(DP_TRAINING_LANE0_SET, lt.train_set, dpsink.lanes);
}

static void set_pattern(int tp)
{
    radeon_dp_encoder(tp == DP_TRAINING_PATTERN_1 ? ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN1
                                                  : ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN2);
    dpcd_writeb(DP_TRAINING_PATTERN_SET, (uint8_t)tp);
}

static int read_status(void)
{
    return radeon_dp_read(DP_LANE0_1_STATUS, lt.status, DP_LINK_STATUS_SIZE);
}

/* The sink's polling interval: 100 us (clock recovery) or 400 us
 * (equalisation) unless its DPCD names a longer one, in 4 ms steps. */
static void train_delay(uint32_t default_us)
{
    uint8_t interval = dpsink.dpcd[DP_TRAINING_AUX_RD_INTERVAL] & 0x7F;
    if (interval)
        mdelay(interval * 4u);
    else
        udelay(default_us);
}

static int train_clock_recovery(void)
{
    set_pattern(DP_TRAINING_PATTERN_1);
    memset(lt.train_set, 0, sizeof(lt.train_set));
    update_vs_emph();
    udelay(400);
    int tries = 0;
    uint8_t voltage = 0xFF;
    for (;;) {
        train_delay(100);
        if (read_status() < 0) {
            klog("radeon: DisplayPort link status unreadable\n");
            return -1;
        }
        if (clock_recovery_ok())
            return 0;
        int lane;
        for (lane = 0; lane < dpsink.lanes; lane++)
            if (!(lt.train_set[lane] & DP_TRAIN_MAX_SWING_REACHED))
                break;
        if (lane == dpsink.lanes) {
            klog("radeon: DisplayPort clock recovery reached the highest swing\n");
            return -1;
        }
        if ((lt.train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK) == voltage) {
            if (++tries == 5) {
                klog("radeon: DisplayPort clock recovery tried 5 times\n");
                return -1;
            }
        } else {
            tries = 0;
        }
        voltage = lt.train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK;
        adjust_train();
        update_vs_emph();
    }
}

static int train_channel_eq(void)
{
    set_pattern(DP_TRAINING_PATTERN_2);
    for (int tries = 0;; tries++) {
        train_delay(400);
        if (read_status() < 0) {
            klog("radeon: DisplayPort link status unreadable\n");
            return -1;
        }
        if (channel_eq_ok())
            return 0;
        if (tries > 5) {
            klog("radeon: DisplayPort channel equalisation failed after 5 tries\n");
            return -1;
        }
        adjust_train();
        update_vs_emph();
    }
}

int radeon_dp_link_train(int panel_mode)
{
    /* init: the sink powered, told the link's rate and width */
    radeon_dp_set_power(1);
    dpcd_writeb(DP_DOWNSPREAD_CTRL, (dpsink.dpcd[DP_MAX_DOWNSPREAD] & 1) ? DP_SPREAD_AMP_0_5 : 0);
    if (panel_mode == DP_PANEL_MODE_INTERNAL_DP2_MODE)
        dpcd_writeb(DP_EDP_CONFIGURATION_SET, 1);
    uint8_t lanes = (uint8_t)dpsink.lanes;
    if (dpsink.dpcd[DP_DPCD_REV] >= 0x11 && (dpsink.dpcd[DP_MAX_LANE_COUNT] & DP_ENHANCED_FRAME_CAP))
        lanes |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
    dpcd_writeb(DP_LANE_COUNT_SET, lanes);
    dpcd_writeb(DP_LINK_BW_SET, (uint8_t)(dpsink.link_clock / 27000));
    radeon_dp_encoder(ATOM_ENCODER_CMD_DP_LINK_TRAINING_START);
    dpcd_writeb(DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_DISABLE);

    int r = train_clock_recovery();
    if (r == 0)
        r = train_channel_eq();

    /* finish, whether it worked or not */
    udelay(400);
    dpcd_writeb(DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_DISABLE);
    radeon_dp_encoder(ATOM_ENCODER_CMD_DP_LINK_TRAINING_COMPLETE);
    klog("radeon: DisplayPort link %s: %d lane%s at %u.%02u GHz, swing %u, pre-emphasis %u\n",
         r == 0 ? "trained" : "training FAILED", dpsink.lanes, dpsink.lanes == 1 ? "" : "s",
         dpsink.link_clock / 100000, dpsink.link_clock / 1000 % 100,
         lt.train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK,
         (lt.train_set[0] & DP_TRAIN_PRE_EMPHASIS_MASK) >> DP_TRAIN_PRE_EMPHASIS_SHIFT);
    return r;
}
