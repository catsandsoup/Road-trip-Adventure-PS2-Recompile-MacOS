# SNDMOD.IRX — structure (for native replacement)

Source of truth: the user's `SNDMOD.IRX` (root and `SOUND/` copies; RTAO pins SHA-256 `9cb20bf4…9d55`).
The module ships with its symbol table and `.mdebug` stabs, so the function names below are the
original ones (recovered by `game/tools/ghidra/DumpFunctions.java`, Ghidra 12.1.4 + EE plugin).

## Threads
| Thread | Role |
| --- | --- |
| `start` → `thread_main` | module entry; `sceSifInitRpc`, `sceSifRegisterRpc`, `sceSifSetRpcQueue`, `sceSifRpcLoop` → `sifrpc_main` |
| `thread_sddr` | sound worker. Woken by `SetAlarm` callback `wakeup_sddr` (`iWakeupThread`), ~16,666 µs period (RTAO) → runs `procDriver`, `procEngine`, `procRadio`, `procDirect`, `procFade` |
| `thread_cdvd` | radio stream feeder (`sceCdRead`/`sceCdSync` into SPU2 ring via `load_radio_part`) |

## EE-facing command surface (`sifrpc_main`, 1976 bytes)
Dispatches to the `sddr*` handlers:
- lifecycle: `sddrInitSddr`, `sddrStopSddr`, `sddrQuitSddr`, `sddrHeapInit`, `sddrBugsSddr`, `sddrSddrBusy` (status poll), `sddrReadData`
- data upload: `sddrTvbfAdrs`/`sddrTvbfTrns` (TVB instrument bank), `sddrTsqfAdrs`/`sddrTsqfTrns` (TSQ sequence)
- control: `sddrCtrlMono`, `sddrCtrlData`, `sddrMuteData`, `sddrAdsrData`, `sddrHardData`, `sddrSoftData`, `sddrSongData`, `sddrStopMode`
- engine: `sddr_EngineInit/Vols/Rpms/Play/Stop/Mute/Loud`
- radio: `sddr_RadioInit/Vols/File/Tune/Play/Stop/Mute/Loud`

## Sequencer / voice core
`reqmus`, `reqse`, `request_que`, `play`, `step_t`, `data_jump`, `data_end`, `tempo`, `key_on`, `key_off`,
`key_pitch`, `pitch`, `pitch_cent`, `chgfreq`, `chgvol`, `chgrev`, `chgtone`, `tone_chg`, `panpot`, `volume`,
`mutevol`, `pri_chg`, `ext_chan`, `set_cancel(_mode)`, `set_volume(_mode)`, `set_reverb(_mode)`, `set_rev_mode`,
`set_rev_depth`, `load_tvbf`, `load_tsqf`, `load_radio`, `load_radio_part`, `vol_Engine`, `vol_Radio`,
tables `freqtbl` (pitch), `sd_s_adsr*`.

## IOP imports used
LIBSD: `sceSdInit`, `sceSdSetParam`, `sceSdSetSwitch`, `sceSdSetAddr`, `sceSdSetCoreAttr`, `sceSdProcBatchEx`,
`sceSdVoiceTrans`, `sceSdBlockTrans`, `sceSdVoiceTransStatus`, `sceSdBlockTransStatus`, `sceSdSetEffectAttr`,
`sceSdGetEffectAttr`. CDVD: `sceCdRead`, `sceCdSync`, `sceCdDiskReady`. Kernel: thread/alarm/intr/sysmem, ioman file I/O.

## Native replacement plan (Phase 4) — superseded by AUDIO_ENGINE.md

Implemented as described in `AUDIO_ENGINE.md`, except step 5: the differential test against the
real IRX under the IOP emulator was **not** done; verification used RTAO's PAL witness values instead.

1. Implement `sifrpc_main` as a host RPC route for SNDMOD's SID with the same command numbers.
2. Port the sequencer core 1:1 from the IRX disassembly into C++ (`game/audio/`), with RTAO's TSQ/TVB/VAG
   decoders and recovered timing as cross-checks.
3. Replace LIBSD/SPU2 with a native 48-voice ADPCM mixer (ADSR, pitch, pan, reverb), output via SDL/CoreAudio.
4. `thread_sddr` becomes a host audio-clock tick at the recovered 16,666 µs period; `thread_cdvd` becomes
   async reads from the user's disc image.
5. Differential test: run the real IRX under the bring-up IOP emulator and the native engine on the same
   command log; compare voice parameter streams (key-on, pitch, volume, ADSR) per tick.

---

# Recovered RPC protocol (from `sifrpc_main` @ 0x7ea8)

Server: `sceSifRegisterRpc(sid 0x01234567, sifrpc_main, rpcGetArg[4 KiB])`. The function number
selects a handler; `work` is always `&sddrwork`. Handlers that return `puts` (rpcPutArg, 4 KiB)
send a reply; all others return NULL (no receive transfer). All payload fields are little-endian
`int32` words of the 64-byte send buffer unless noted. Recovered with llvm-objdump + a symbolic
pass over the -O0 code (`work/audio/sndmod.asm`, `sndmod_pseudo.c`, not tracked).

| fn | handler | payload | reply / effect |
| --- | --- | --- | --- |
| 0x00 | progInitWork(0); progSpu2Zero; sddrInitSddr | – | zero 2 MiB SPU RAM, set the effect work areas, effect mode off, then run the 46-entry init batch `bats.24` (mixer/master/effect volumes, and resets every voice's volume, pitch, start address, ADSR and mix routing, then key-off). The table itself is read from the user's IRX at run time. |
| 0x01 | progInitWork(1); sddrStopSddr | – | clears channel work, batch `bats.27` (ADSR reset + KOFF all) |
| 0x02 | sddrHeapInit | – | no-op |
| 0x03 | sddrQuitSddr | – | no-op |
| 0x04 | sddrTvbfAdrs | {bank, spuAddr} | `tvbtbl[bank]` = SPU byte address of the bank's ADPCM data |
| 0x05 | sddrTsqfAdrs | {bank, offset} | `tsqtbl[bank] = &tsqbuf[offset]` (tsqbuf = 256 KiB) |
| 0x06 | sddrTvbfTrns | SDDRfile {bank, lsn, sectors, name[52]} | if different from the slot's last file: `loadTvbf[bank].load = 1` (thread_cdvd loads) |
| 0x07 | sddrTsqfTrns | SDDRfile | same for `loadTsqf[bank]` |
| 0x08 | sddrCtrlMono | {0, mono} / {1} | set mono_flag (re-applies engine/radio volumes) / reply word0 = mono_flag |
| 0x09 | sddrCtrlData | {0, num, vol} / {1, num, rev} | set_volume / set_reverb on matching SE+BGM channels (`num` = bank<<8 \| index; index 0 = whole bank) |
| 0x0a | sddrMuteData | {op, core0mask, core1mask} | per-voice output mute: 0 clear, 1 set, 2 assign |
| 0x0b | sddrAdsrData | {op, adsr1, adsr2} | 0 per-tone ADSR, 1 global ADSR, 2 global ADSR := given |
| 0x0c | sddrHardData | DIWORK (56 bytes) | copied into `diwork[channel]` (only 0..7 are processed) |
| 0x0d | sddrSoftData | {data} (rest of buffer is garbage) | `data & 0x8000`: set_cancel(data & 0xfff). Else bank=(data>>8)&0xf, index=data&0xff: directory entry at `tsqtbl[bank]+index*4` = {u16 prio, u16 seqOffset}. prio≠0 → reqse(prio, …, pan = data>>16 = panL<<8\|panR) if the bank is loaded; prio=0 → music: cancel BGM, bgmbuf=req, bgmset=2 (start) |
| 0x0e | sddrSongData | {cmd, arg} | jump table @0x8aa0: 0 select (bgmbuf = arg & 0xfff, bgmset=0, fade off), 1 start, 2 stop, 3 mute, 4 unmute, 5 fade out, 6 fade in (bits consumed by request_que each tick) |
| 0x0f | sddrStopMode | {mode} | set_cancel_mode(mode): 0 = all BGM channels, else all SE channels |
| 0x10 | sddrSddrBusy | – | reply word0 = 1 while any TVB/TSQ slot has load==1, else 0 |
| 0x1f | sddrReadData | – | reply: 2×32 bytes read back from SPU2 capture memory (block-trans read) |
| 0x20 | sddr_EngineInit | – | both engines: flag_play=1, vol 0x3fff, voice VOL sweep 0x8013 (clears 72 bytes per engine — engine[1] overruns into the first 36 bytes of the radio work) |
| 0x21 | sddr_EngineVols | {engine, voll, volr} | voices 2e,2e+1 volumes |
| 0x22 | sddr_EngineRpms | {engine, type, disc, rpms, efct} | per-tick engine state (type → tone pair, disc = which of the pair sounds, efct = reverb send) |
| 0x23/24 | sddr_EnginePlay / Stop | {engine} | flag_play 2 / 1 |
| 0x25/26 | sddr_EngineMute / Loud | {engine} | stop & remember / resume |
| 0x30 | sddr_RadioInit | – | radio work reset, voices 4..7 VOL sweeps |
| 0x31 | sddr_RadioVols | {voll, volr} | voices 4,5 (left stream) / 6,7 (right stream) |
| 0x32 | sddr_RadioFile | SDDRfile with bank n | `prog_file[n>>1][n&1]`; PAL registers n=0,1 → 1CH L/R, 2,3 → 3CH, 4,5 → 2CH |
| 0x33 | sddr_RadioTune | {tune, time} | play_tune, play_time (ticks), force reload |
| 0x34/35 | sddr_RadioPlay / Stop | – | |
| 0x36/37 | sddr_RadioMute / Loud | – | |
| 0xfff0 | sddrBugsSddr | – | reply: raw chwork (768 bytes) |

Observed PAL boot→title→menu→first scene: 0x00, 0x04×4, 0x05×4, 0x32×6, 0x06×4, 0x07×4,
0x0d (SE), 0x0e (select 0x104 + start), 0x0f, 0x20, 0x21. Engine RPM/radio play commands do not
occur on that route (they are exercised by the offline harness instead).

## Data structures (stabs)
`CHWORK` (16 B: hardware voice cache: active/pri, mode (0 BGM, 1 SE owner), key (1 off, 2 on),
rev_flag, tone, freq, vol_L, vol_R, channel), `MUWORK` (28 B, sequence channel: active (= request
index), bank, mode, pri, tone, pan_L, pan_R, vol_L, vol_R, cent, freq, data_adr, data_cnt, volume,
rev_flag, main_vol), `DIWORK` (56 B direct voice request: mask, channel, key, ssa, adsr1, adsr2,
bank, wave, offset, pitch, vmix, vmixe, voll, volr; mask bits 1 key, 2 ssa, 4 adsr1, 8 adsr2,
0x10 tone, 0x20 pitch, 0x40 vmix, 0x80 vmixe, 0x100 voll, 0x200 volr), `SDDRfile`, `SDDRload`,
`SDDRengine`, `SDDRradio`, `SDDRwork` (1080 B). Exact offsets are static_asserted in
`ps2xIOP/src/modules/sndmod/sndmod_driver.h`.

## Voice map
Voice channel c (0..47) → libsd entry `((c % 24) << 1) | (c / 24)`. 0..3 engines (2 per engine:
"mute"/"loud" sample), 4..7 radio (4/5 left, 6/7 right, double-buffered), 8..43 BGM (36 TSQ
channels), 8..47 SE (allocated by priority over chwork[8..47].active).

## Sequencer (play @0x5294, one call per channel per tick)
`data_cnt` countdown; opcodes: 0x00–0x7f wait n ticks (0 = 65536), 0x80–0xdf key-on with
`freqtbl[op & 0x7f]` (×cent/4096 if cent≠0), E0 volume, E1 pan L,R, E2 tone (s8 | bank<<8, clears
cent), E3 two-byte no-op ("tempo"), E4 pitch, E5 key-on with explicit pitch, E6 reverb mode, E7
reverb depth, E8/E9 reverb send on/off, EA cent (Q12), F0 key-off, F1 priority, F8 relative jump
(s16 from post-operand pc), F9 fork channel into an SE slot, anything else (FF) ends the channel.
Volume = volume·pan·main_vol/256 (C division). No BGM file uses E6/E7.

## Tables
`freqtbl` (73 × u16 @ 0x8acc) = floor(4096·2^((i−48)/12)), entry 72 clamped to 0x3fff.
`data.169` (.data 0x8d44, 5×3 int32): engine rpm→pitch curve of {rpm, bottom pitch, top pitch} points;
pitch = btm + (rpm−r0)(top−btm)/(r1−r0) (linear interpolation between points).
`wave.168` (.data 0x8d04, 8×2 int32): per engine type, a {mute, loud} pair of tone numbers in bank 0.
Both are read from the user's SNDMOD.IRX at run time (values intentionally not reproduced here).
TVB header: u32 wave offset[256], u32 rsda[256]; ADSR = (~rsda & 0xdffffff0) | (rsda & 0xf)
(low half ADSR1, high half ADSR2); ADPCM payload from byte 0x800.
Radio: tune·120 + play_time/1800 selects a 30 s part (205,714 bytes of 12 kHz mono PS-ADPCM, +64
header); part buffers at SPU 0x5000 + ((side·2 + disc) << 18), pitch 0x400, start offset
114·(time in part) bytes; play_time wraps at 216,000 ticks (1 hour).
