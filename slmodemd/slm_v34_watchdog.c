/*
 * slm_v34_watchdog.c -- an observer (and, optionally, an override) around the
 * one blob entry point that decides whether a V.34 call lives or dies.
 *
 * WHY THIS EXISTS
 *
 * `vpcm_run` (inside dsplibs.o) calls VPcmV34Progress once per 48-sample
 * fragment and switches on its return value.  Reading that switch out of the
 * disassembly gives the complete life-or-death table:
 *
 *     ret  0        "Re-starting phase II", and RESETS the training watchdog
 *     ret  1        "Phase II completed !!!"
 *     ret  2,3      no change
 *     ret  4,5      LINK ESTABLISHED  -> reads the DP and the rates, CONNECT
 *     ret  6,7      still training
 *     ret  8,9,16   FATAL -> "vpcm: Link Error" -> modem_hup -> NO CARRIER
 *     ret 10        "Same Line Verification Status"
 *     ret 11..15    no change
 *
 * and the watchdog beside it:
 *
 *     if (ret == previous_ret && ret == 0 && ++counter > 3000)
 *             "vpcm: train timeout!" -> FATAL
 *
 * 3001 fragments of 48 samples at 9600 Hz is 15.005 s, which is exactly the
 * 15.06 s at which every `train timeout!` in the journal fires.
 *
 * The return value is simply *(int *)(obj + 4).  The phase the blob dispatches
 * on internally is *(int *)(obj + 0).  The elapsed-sample counter that
 * VPcmV34SetTimeOut(obj, seconds) initialises is at obj + 0x238, and its limit
 * (seconds * 9600) at obj + 0x23c.
 *
 * NOTHING IS PATCHED.  dsplibs.o emits its call to VPcmV34Progress as an
 * R_386_PC32 relocation against the global symbol, so weakening the blob's
 * definition and providing a strong one here is enough -- the same mechanism
 * apply_v34_info1a_hook.sh already uses.  apply_v34_progress_hook.sh asserts
 * the call site actually reaches this wrapper.
 *
 * Default behaviour is OBSERVE ONLY.  The override is off unless asked for.
 *
 * WHAT STATUS 9 ACTUALLY IS
 *
 * Read out of v34handshak's tail (dsplibs.o .text+0x62a92..0x62ac5), with
 * [esp+0x78] holding obj+4:
 *
 *     if (elapsed_samples > timeout_limit)        *status = 8;   // 45 s
 *
 *     if ((short)metric < obj->lowsig_thresh)     obj->lowsig_count++;
 *     else                                        obj->lowsig_count = 0;
 *     if (obj->lowsig_count > 0x257f)             *status = 9;   // 9600 = 1.000 s
 *
 * v34handshak is called ONCE PER SAMPLE from the phase-0 loop, so 9600 counts
 * is exactly one second at the 9600 Hz DSP rate, and the counter is
 * CONSECUTIVE -- a single good sample resets it.  Status 9 therefore means
 * "the receiver's signal metric stayed below its threshold for one unbroken
 * second during the handshake", nothing more.  It is a watchdog, not a decode
 * failure, and it is latched: once v34handshak sets it, it sets it again on
 * every following sample, which is why refusing the return value alone is
 * useless (measured -- see the write-up linked in the README).
 *
 * The threshold is at obj+0x230 and the counter at obj+0x234, both writable
 * from here before every fragment.  The metric itself is a short at obj+0x398:
 * v34handshak keeps obj+0x264 in [esp+0x74] and reads +0x134 off it.  Reading
 * it here turns "how far below 101 are we, and for how long" from a threshold
 * sweep into a direct measurement.
 *
 *   SLM_V34_TRACE=1        log phase/status transitions (default 1)
 *   SLM_V34_LOWSIG_EXTEND=K
 *                             the conservative form of the same fix: leave the
 *                             threshold alone and instead let the counter be
 *                             zeroed at most K times per call, which makes the
 *                             deadline (K+1) seconds of CONTINUOUS low signal
 *                             instead of one.  A genuinely dead line still ends
 *                             the call.  Measured requirement is 50 ms, so K=1
 *                             leaves a second of margin.  0 = off (default).
 *   SLM_V34_METRIC=N      trace the signal metric every N fragments
 *                             (N=20 is 100 ms).  0 = off, the default.
 *   SLM_V34_TRAINEXTEND=K zero vpcm_run's own train-timeout counter at most
 *                             K times per call, giving (K+1) x 15.005 s of
 *                             acquisition instead of one, without patching the
 *                             0xbb8 immediate.  vpcm_run keeps that counter at
 *                             dsp_priv+0x20 and hands us dsp_priv+0x2c, so it
 *                             is obj-0xc from here.  0 = leave alone (default).
 *                             The 45 s session timeout still bounds the call,
 *                             so K above 2 buys nothing.
 *   SLM_V34_LOWSIG=N      force obj->lowsig_thresh to N before every
 *                             fragment.  EVERY fragment is not belt-and-braces:
 *                             VPcmV34SetMinimumSigLevel writes the threshold
 *                             from its table, and VPcmV34InitiateRetrain calls
 *                             it, so a one-time write would be silently undone
 *                             by the first retrain and the setting would look
 *                             like it had simply failed on some calls.
 *                             A large negative value can never be
 *                             met by a 16-bit metric, so the counter resets on
 *                             every sample and status 9 can never fire.  The
 *                             call is still bounded: the 45 s SetTimeOut
 *                             watchdog (status 8) and vpcm_run's 15 s
 *                             train-timeout both remain.
 *   SLM_V34_RENEG_TARGET=B
 *                             after the link comes up, if the negotiated RX bit
 *                             rate is below B, ask the blob to renegotiate one
 *                             rate step UP, and keep asking while it is still
 *                             below B.  See the note on modes below.  0 = off.
 *   SLM_V34_RENEG_MAX=K   at most K attempts per call (default 2).  Each
 *                             one re-enters the handshake and is a chance to
 *                             lose a working link, so this is a hard cap.
 *   SLM_V34_RENEG_WAIT=N  fragments to let the link settle before the first
 *                             attempt, and between attempts (default 400 = 2 s).
 *   SLM_V34_RETRAIN_THRESH=N
 *                             force the blob's own retrain threshold to N
 *                             (and renegDownthresh to 2N/3, the ratio it uses
 *                             itself) before every fragment.  This is the
 *                             "keep the rate AND stop the retraining" lever.
 *
 *                             The threshold is armed FROM the chosen rate --
 *                             100 at 33,600, 262 at 28,800, 732 at 24,000 --
 *                             and then compared against every subsequent
 *                             `equerr` sample.  Measured over 2768 data-mode
 *                             samples the distribution is median 81, p90 228,
 *                             p99 1220, so at 33,600 **38.6% of samples are
 *                             above the trigger** and a retrain is available
 *                             several times a second.  Lowering the rate buys
 *                             margin by giving up bits; this buys the same
 *                             margin by giving up the retrain.
 *
 *                             The fields are 16-bit, at obj+0x4b6 (retrain),
 *                             obj+0x4b8 (renegDown) and obj+0x4ba (renegUp) --
 *                             read out of the code that logs them at
 *                             .text+0x676f3, where [esp+0x74] is obj+0x264 and
 *                             it prints +0x252/+0x254/+0x256 off that.
 *
 *                             RISK, stated plainly: if the excursions are real
 *                             channel events rather than estimator noise, a
 *                             suppressed retrain means V.42 retransmitting
 *                             instead, so the link goes slow rather than
 *                             breaking.  ops/v34-endtoend.py measures exactly
 *                             that -- http bps AND hold -- so score it there
 *                             and not on CONNECT.  0 = off.
 *   SLM_V34_RETRAIN_DOWN=K
 *                             after K retrains on an established link, step the
 *                             rate DOWN one notch (mode 2) instead of letting
 *                             it keep retraining at the same one.  This is the
 *                             opposite trade to RENEG_TARGET and, since the
 *                             establishment fix, the more valuable one: a held
 *                             link on this path retrains every 20-90 s, each
 *                             costing ~10 s, and the blob arms its retrain
 *                             threshold FROM the chosen rate -- 100 at 33,600,
 *                             262 at 28,800, 732 at 24,000 -- against a
 *                             data-mode equerr of ~55.  Stepping down buys
 *                             margin at the cost of 2,400 bps.  0 = off.
 *                             Capped by SLM_V34_RENEG_MAX like the other
 *                             direction, because each step is itself a
 *                             handshake re-entry.
 *   SLM_V34_FATAL_TO=N    when the blob returns a fatal 8/9/16, rewrite the
 *                             status to N and return N instead.  Kept only as a
 *                             diagnostic; it does NOT clear the latch.
 *   SLM_V34_FATAL_MAX=K   suppress at most K fatals per call (default 8),
 *                             so a hopeless call still ends.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

extern int VPcmV34GetSNR(void *);
extern int VPcmV34GetCurrentRxBitRate(void *);
extern int VPcmV34GetCurrentRxBaudRate(void *);
extern int VPcmV34GetCurrentRxCarrier(void *);
extern int VPcmV34GetCurrentTxBitRate(void *);
/* VPcmV34InitiateRateRenegotiation(obj, mode), read out of .text+0x6530 with
 * esi = obj+4 and the rate indices at +0x21c (min) +0x220 (max) +0x224 (current)
 * +0x228 (requested):
 *
 *   mode 2, 5   requested = current - 1, i.e. one step DOWN
 *   mode 3      requested = current + 1, i.e. one step UP   <- what we want
 *   0,1,4,>=6   requested = -1, i.e. "renegotiate, no preference"
 *
 * Every path then calls v34handshakinit(obj, 2) and sets *status = 6, which
 * vpcm_run's table treats as "still training" -- NOT as fatal. So this is the
 * in-spec rate renegotiation and not the retrain that this project has already
 * measured killing calls on this path. */
extern void VPcmV34InitiateRateRenegotiation(void *, int);
#define RENEG_UP   3
#define RENEG_DOWN 2

extern int slm_v34_watchdog_orig(void *obj, float *in, float *out, int n,
                                int *rxbits, int *nrx, int *txbits, int *ntx)
        __asm__("VPcmV34Progress__orig");
int slm_v34_watchdog_wrap(void *obj, float *in, float *out, int n,
                         int *rxbits, int *nrx, int *txbits, int *ntx)
        __asm__("VPcmV34Progress");

static int envi(const char *n, int d)
{
        const char *s = getenv(n);
        return (s && *s) ? atoi(s) : d;
}

/* obj + 0x238 is the sample counter VPcmV34SetTimeOut zeroes; at the 9600 Hz
 * DSP rate it is a wall clock in samples since the datapump started. */
#define OBJ_PHASE(o)   (((int *)(o))[0])
#define OBJ_STATUS(o)  (((int *)(o))[1])
/* vpcm_run's own per-call state sits just below the V.34 object it passes us:
 * it holds dsp_priv and calls us with dsp_priv+0x2c. */
#define RUN_LASTRET(o) (*(int *)((char *)(o) - 0x18))   /* dsp_priv+0x14 */
#define RUN_LASTST(o)  (*(int *)((char *)(o) - 0x14))   /* dsp_priv+0x18 */
#define RUN_TRAIN(o)   (*(int *)((char *)(o) - 0x0c))   /* dsp_priv+0x20 */
#define OBJ_METRIC(o)  (*(short *)((char *)(o) + 0x398))
#define OBJ_RETRAIN_T(o)   (*(short *)((char *)(o) + 0x4b6))
#define OBJ_RENEGDOWN_T(o) (*(short *)((char *)(o) + 0x4b8))
#define OBJ_THRESH(o)  (*(int *)((char *)(o) + 0x230))
#define OBJ_LOWCNT(o)  (*(int *)((char *)(o) + 0x234))
#define OBJ_SAMPLES(o) (*(int *)((char *)(o) + 0x238))
#define OBJ_LIMIT(o)   (*(int *)((char *)(o) + 0x23c))

static int is_fatal(int r) { return r == 8 || r == 9 || r == 16; }

int slm_v34_watchdog_wrap(void *obj, float *in, float *out, int n,
                         int *rxbits, int *nrx, int *txbits, int *ntx)
{
        static void *last_obj;
        static int last_phase, last_status, blocks, suppressed;
        static int thresh_said, peak_low, mlo, mhi, extended, trained;
        static int linked_at, renegs, retrains, was_linked;
        int dbg = envi("SLM_V34_TRACE", 1);
        int trace = envi("SLM_V34_METRIC", 0);
        int m;
        int lowsig = envi("SLM_V34_LOWSIG", 1);   /* 1 = leave alone */
        int to  = envi("SLM_V34_FATAL_TO", -1);
        int max = envi("SLM_V34_FATAL_MAX", 8);
        int tex = envi("SLM_V34_TRAINEXTEND", 0);
        int ext = envi("SLM_V34_LOWSIG_EXTEND", 0);
        int ph0, r, ph1, cnt;

        if (obj != last_obj) {          /* new datapump: a new call */
                last_obj = obj;
                last_phase = -1; last_status = -1;
                blocks = 0; suppressed = 0; thresh_said = 0; peak_low = 0;
                extended = 0; trained = 0; mlo = 32767; mhi = -32768;
                linked_at = 0; renegs = 0; retrains = 0; was_linked = 0;
        }
        /* v34handshak fires at > 0x257f, and it runs 48 times between our
         * calls, so zero the counter while there is still a fragment of room. */
        if (ext > 0 && extended < ext && OBJ_LOWCNT(obj) > 0x257f - 96) {
                extended++;
                fprintf(stderr, "V34PROG lowsig extend %d/%d at %.3f s "
                        "(count was %d)\n", extended, ext,
                        blocks * (double)n / 9600.0, OBJ_LOWCNT(obj));
                OBJ_LOWCNT(obj) = 0;
        }
        if (dbg && !thresh_said) {
                thresh_said = 1;
                /* RUN_LASTST should mirror what we returned last fragment; on
                 * the first fragment of a call it is whatever the previous call
                 * left, which is enough to show the offset is not garbage. */
                fprintf(stderr, "V34PROG lowsig threshold %d, timeout %d "
                        "samples (%.1f s), vpcm_run lastret %d lastst %d "
                        "train %d\n", OBJ_THRESH(obj), OBJ_LIMIT(obj),
                        OBJ_LIMIT(obj) / 9600.0, RUN_LASTRET(obj),
                        RUN_LASTST(obj), RUN_TRAIN(obj));
        }
        /* Do this BEFORE the call: vpcm_run increments and tests the counter
         * after we return, so resetting on the way in is what bounds it. */
        if (tex > 0 && trained < tex && RUN_TRAIN(obj) > 3000 - 2) {
                trained++;
                fprintf(stderr, "V34PROG train extend %d/%d at %.3f s "
                        "(count was %d)\n", trained, tex,
                        blocks * (double)n / 9600.0, RUN_TRAIN(obj));
                RUN_TRAIN(obj) = 0;
        }
        if (lowsig != 1)
                OBJ_THRESH(obj) = lowsig;
        {
                /* Same reasoning as the low-signal threshold: the blob rewrites
                 * this whenever it re-chooses a rate, so a one-time write would
                 * be undone by the first renegotiation. */
                int rt = envi("SLM_V34_RETRAIN_THRESH", 0);
                if (rt > 0 && OBJ_RETRAIN_T(obj) != (short)rt) {
                        if (dbg)
                                fprintf(stderr, "V34PROG retrain threshold "
                                        "%d -> %d (renegDown %d -> %d)\n",
                                        OBJ_RETRAIN_T(obj), rt,
                                        OBJ_RENEGDOWN_T(obj), rt * 2 / 3);
                        OBJ_RETRAIN_T(obj) = (short)rt;
                        OBJ_RENEGDOWN_T(obj) = (short)(rt * 2 / 3);
                }
        }
        ph0 = OBJ_PHASE(obj);
        r = slm_v34_watchdog_orig(obj, in, out, n, rxbits, nrx, txbits, ntx);
        ph1 = OBJ_PHASE(obj);
        blocks++;

        /* The low-signal counter is consecutive, so its peak within a call is
         * the closest that call ever came to the one-second deadline. */
        cnt = OBJ_LOWCNT(obj);
        if (cnt > peak_low) peak_low = cnt;

        m = OBJ_METRIC(obj);
        if (m < mlo) mlo = m;
        if (m > mhi) mhi = m;
        if (trace > 0 && blocks % trace == 0) {
                fprintf(stderr, "V34METRIC %.3f s  now %d  window [%d,%d]  "
                        "thresh %d  lowcnt %d  status %d\n",
                        blocks * (double)n / 9600.0, m, mlo, mhi,
                        OBJ_THRESH(obj), cnt, r);
                mlo = 32767; mhi = -32768;
        }

        if (dbg && (ph1 != last_phase || r != last_status)) {
                fprintf(stderr,
                        "V34PROG blk %d (%.3f s) phase %d->%d status %d->%d "
                        "lowcnt %d (peak %d) samples %d/%d\n",
                        blocks, blocks * (double)n / 9600.0, last_phase, ph1,
                        last_status, r, cnt, peak_low,
                        OBJ_SAMPLES(obj), OBJ_LIMIT(obj));
                last_phase = ph1; last_status = r;
        }

        if (is_fatal(r)) {
                fprintf(stderr,
                        "V34PROG FATAL status %d at blk %d (%.3f s) "
                        "phase %d->%d lowcnt %d thresh %d "
                        "rxbaud %d rxbps %d txbps %d carrier %d "
                        "snr %d samples %d/%d\n",
                        r, blocks, blocks * (double)n / 9600.0, ph0, ph1,
                        cnt, OBJ_THRESH(obj),
                        VPcmV34GetCurrentRxBaudRate(obj),
                        VPcmV34GetCurrentRxBitRate(obj),
                        VPcmV34GetCurrentTxBitRate(obj),
                        VPcmV34GetCurrentRxCarrier(obj),
                        VPcmV34GetSNR(obj),
                        OBJ_SAMPLES(obj), OBJ_LIMIT(obj));
                if (to >= 0 && suppressed < max) {
                        suppressed++;
                        fprintf(stderr, "V34PROG override: %d -> %d (%d/%d)\n",
                                r, to, suppressed, max);
                        OBJ_STATUS(obj) = to;
                        last_status = to;
                        r = to;
                }
        }
        /* ---- rate renegotiation -------------------------------------
         * The bit rate is a table lookup on ONE equerr sample from an
         * estimator that swings 79->714 inside a four-second window, so a low
         * rate is often a bad draw rather than a verdict on the channel. This
         * takes another draw. It is bounded hard: a renegotiation re-enters the
         * handshake, and a link that is up and slow beats a link that is gone.
         */
        if (r == 4 || r == 5) {
                if (!linked_at) linked_at = blocks;
                was_linked = 1;
        } else if (was_linked && (r == 0 || r == 6)) {
                /* An established link that has gone back into the handshake:
                 * that is a retrain, and it is the thing costing ~10 s a time. */
                was_linked = 0;
                retrains++;
                {
                        int k = envi("SLM_V34_RETRAIN_DOWN", 0);
                        int rmax = envi("SLM_V34_RENEG_MAX", 2);
                        if (k > 0 && retrains >= k && renegs < rmax) {
                                renegs++;
                                fprintf(stderr, "V34PROG retrain %d at %.3f s: "
                                        "stepping the rate DOWN (%d/%d)\n",
                                        retrains, blocks * (double)n / 9600.0,
                                        renegs, rmax);
                                VPcmV34InitiateRateRenegotiation(obj, RENEG_DOWN);
                        } else if (dbg) {
                                fprintf(stderr, "V34PROG retrain %d at %.3f s\n",
                                        retrains, blocks * (double)n / 9600.0);
                        }
                }
        }
        {
                int target = envi("SLM_V34_RENEG_TARGET", 0);
                int rmax   = envi("SLM_V34_RENEG_MAX", 2);
                int wait   = envi("SLM_V34_RENEG_WAIT", 400);
                if (target > 0 && linked_at && renegs < rmax &&
                    blocks - linked_at > wait * (renegs + 1)) {
                        int bps = VPcmV34GetCurrentRxBitRate(obj);
                        if (bps > 0 && bps < target) {
                                renegs++;
                                fprintf(stderr, "V34PROG reneg %d/%d at %.3f s: "
                                        "rx %d below target %d, stepping up\n",
                                        renegs, rmax,
                                        blocks * (double)n / 9600.0, bps, target);
                                VPcmV34InitiateRateRenegotiation(obj, RENEG_UP);
                        } else if (bps >= target) {
                                /* Stop asking. Leaving linked_at set would have
                                 * us re-check every fragment for the rest of the
                                 * call for no reason. */
                                renegs = rmax;
                        }
                }
        }
        return r;
}
