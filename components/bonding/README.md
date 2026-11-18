# DVB-T bonding over two 40 kHz channels

Can one transmitter carry a single programme over two 40 kHz DVB-T channels and deliver **at least 1.7 times the usable transport capacity of one 40 kHz channel**? To investigate, we tested two ways of bonding channels at the MPEG-TS level. In either mode, a viewer or listener can tune to either channel. Linrad DVB automatically identifies the partner channel and the bonding mode.

The question is also relevant to radio amateurs in countries that limit transmission bandwidth on VHF. What counts as one transmission? On the air, there are two DVB-T signals on different frequencies, each of which a receiver can tune to and demodulate independently. For a viewer receiving both, they carry one programme. In *Separate lanes* mode, one person can listen to the audio on channel B while someone else watches the video on channel A. Whether to call this two transmissions or one combined broadcast remains an open question. The experiment does not address how local bandwidth rules apply.

## Bonding at the MPEG-TS level

The **bonding splitter** sits in the transmitter, between the programme multiplexer and DVB-T modulation. It takes one MPEG transport stream (MPEG-TS) and produces two transport streams, one for each 40 kHz channel. The **bonding merger** sits in the receiver, after DVB-T demodulation and error correction on each channel. It combines the two received transport streams into one MPEG-TS for playback, recording or network output.

```text
                               ┌─ TS A → DVB-T channel A (40 kHz) → TS A ─┐
Source TS → bonding splitter ──┤                                          ├─ bonding merger → Output TS
                               └─ TS B → DVB-T channel B (40 kHz) → TS B ─┘
              transmitter                                                       receiver
```

## Two ways to use the channels

| | Shared (distribution 0) | Separate lanes (distribution 2) |
| --- | --- | --- |
| What does the splitter send? | It distributes source packets and recovery data across TS A and TS B. | TS A carries a standalone video programme. TS B carries a standalone audio programme. |
| What can you receive on one channel? | Part of the bonded stream. The receiver uses both channels to reconstruct the full programme. | **You can receive and decode either channel independently with an ordinary DVB-T decoder that supports 40 kHz bandwidth.** Tune to A for video alone or to B for audio alone. |
| What does the merger do? | It restores the original packet order and uses the available recovery data to reconstruct missing packets. | It combines the video and audio into one playable TS using their common timeline, preserving the encoded media and their presentation timestamps. |

In **Separate lanes** mode, each channel carries its own programme tables and timing information. This mode uses no XOR protection. If B drops out, video on A can continue. Anyone receiving only B can listen directly to the audio programme. To produce combined output, the merger needs the programme on A. It rebuilds the programme tables and continuity counters.

### Protection profiles in Shared mode

The splitter bundles up to seven MPEG-TS packets into one **source symbol** and computes one XOR parity symbol for each group of source symbols. If one source symbol is missing, the merger can reconstruct its packets exactly, provided the parity symbol and all other symbols in the group arrive in time. The transmitter announces the selected profile in the programme map table (PMT), and the receiver applies it automatically.

| Profile | What does the splitter send per full group? | What can the merger recover? | Maximum source TS bitrate under test settings |
| --- | --- | --- | ---: |
| **XOR8** | 8 source symbols + 1 parity symbol | One missing source symbol per group of 8. | 47,541 bit/s |
| **XOR16** (default) | 16 source symbols + 1 parity symbol | One missing source symbol per group of 16. The lower parity overhead leaves more capacity for the source TS. | 50,337 bit/s |
| **Hybrid16** | 16 source symbols + 1 parity symbol + 1 extra copy on the opposite lane. The splitter selects the symbol to copy by inspecting the PES/NAL content. | If the selected symbol is missing, its copy can replace it. Parity can then recover one other missing symbol in the same group. | 47,541 bit/s |

These limits apply at 30,160 bit/s per channel with a 2-second announcement interval. At those settings, only XOR16 can carry the 50,100 bit/s test source. For other settings, use `dvb_bond_source_limit_v1()` to obtain the source bitrate limit. [Public C API](include/dvb_bond.h)

All three profiles support **interleaving**. The splitter alternates between symbols from different recovery groups. A brief signal outage can then erase one symbol in each of several groups, allowing the merger to repair each group independently. Spreading symbols over a longer interval may require a longer playout buffer. At the playout deadline, the merger substitutes null packets for any source packets still missing.

## How the TS packets announce bonding

On each lane, the splitter periodically sends a Programme Association Table (PAT) on PID `0x0000`. The PAT identifies the PID of that lane’s PMT (table ID `0x02`). The bonding announcement is in the PMT’s **programme-level descriptor loop** (`program_info_length`). A receiver can therefore discover the pair from either lane before decoding any media. Ordinary 188-byte TS packets retain their usual headers. The default announcement interval is 2 seconds.

The splitter uses the three private descriptors below. To inspect a capture, follow the PAT to the PMT, verify the PMT section CRC, then look for the descriptor tag, length, ASCII signature and version together. The tags are local to this experimental protocol. All offsets below are decimal byte offsets **within the descriptor payload**, starting immediately after the one-byte tag and one-byte length. Multibyte integers are unsigned and big-endian.

### Shared mode — two descriptors in each lane’s PMT

| Payload offsets | Discovery descriptor (`0x80`, 64-byte payload) |
| --- | --- |
| 0–7 | Bytes 0–3 are ASCII `DBNA`. Byte 4 is version `1`, byte 5 is length `64`, byte 6 is the transmitting lane (`0` = A, `1` = B), and byte 7 is the lane count (`2`). |
| 8–27 | Bond ID (8 bytes), session ID (8 bytes), configuration revision (4 bytes). |
| 28–31 | One byte each for source symbols per full group (8 or 16), XOR parity symbols per group (1), TS packets per full symbol (7), and RF profile (1 = narrowband DVB-T, QPSK, 2K, FEC 1/2, GI 1/32). |
| 32–47 | This lane’s nominal centre frequency followed by its partner’s frequency (8 bytes each, Hz). Lane B advertises the frequencies in the reverse order to lane A. |
| 48–59 | Channel bandwidth in Hz (40,000), source TS bitrate in bit/s, announcement interval in ms (4 bytes each). |
| 60–63 | CRC-32/ISO-HDLC over payload bytes 0–59, in addition to the enclosing PMT section CRC. |

The **transport descriptor** (tag `0x81`, 12-byte payload) supplies the remaining parameters. Bytes 0–3 are ASCII `DBNP`. Byte 4 is the transport version (the current splitter writes `3`), byte 5 is the protection profile (`1` = XOR8, `2` = XOR16, `3` = Hybrid16), byte 6 is the interleave depth in groups (1–8), and byte 7 is the distribution (`0` = Shared). Bytes 8–11 give the TS bitrate of one lane in bit/s.

The Shared PMT also lists a private PES stream (stream type `0x06`, PID `0x1FFE` in this splitter). That stream carries the `DBND` data and repair records. The receiver learns how to interpret them from the two PMT descriptors. [Shared PMT writer and reader](src/dvb_bond.c) · [Discovery descriptor encoder and CRC](src/bond_discovery.c)

### Separate lanes — one descriptor in each lane’s PMT

| Payload offsets | Native lane descriptor (`0x82`, 60-byte payload) |
| --- | --- |
| 0–7 | Bytes 0–3 are ASCII `DBNA`. Byte 4 is version `4`, byte 5 is the lane (`0` = video A, `1` = audio B), byte 6 is the distribution (`2` = Separate lanes), and byte 7 is the repair profile (`0` = none). |
| 8–23 | Bond ID and session ID (8 bytes each). |
| 24–39 | Nominal centre frequencies of A and B (8 bytes each, Hz), **in A/B order on both lanes**. The lane byte identifies which frequency belongs to the received signal. |
| 40–55 | Source TS bitrate (bit/s), per-lane TS bitrate (bit/s), configuration revision, announcement interval (ms), all 4 bytes each. |
| 56–59 | Four reserved zero bytes. The PMT section CRC covers the descriptor. |

The PMT still lists the native video or audio elementary stream, so an ordinary decoder can ignore the private descriptor and play that lane. A bonding receiver uses the first valid announcement to find the partner. For example, with A at 144.600 MHz and B at 144.650 MHz, tuning to B first gives a partner offset of **−50 kHz**. To confirm the pair, the receiver checks that the other lane advertises the same pair, session and configuration. In Separate lanes mode it also checks that the programme number and PMT PID agree. [Native PMT writer and receiver checks](src/bond_native.c)

## Automatic detection in Linrad DVB

1. Select DVB-T with a bandwidth of 40 kHz and tune to the centre of either A or B. Both channels must fall within the IQ bandwidth of the same SDR input or recording. The trials used 50 kHz channel spacing and an IQ sample rate of 250 kS/s.
2. The receiver reads the bonding announcement from the PMT on the tuned channel. The announcement identifies the session, mode and partner frequency. Linrad DVB starts a second decoder in the same IQ input at the announced offset, typically +50 or −50 kHz.
3. When the partner channel announces a matching pair, the indicator shows **Paired**. The merger provides one TS for playback, recording or network output. The default playout buffer is eight seconds.

To watch or listen to a lane on its own, select **Mode → Disable automatic DVB-T bonding** before starting reception. The receiver still detects the announcement and passes the tuned channel’s TS straight through. You can activate the partner decoder later using the **Bonding** indicator. [Receiver operation and status fields (in Dutch)](../../docs/dvbt-bonding.md)

## Experimental results

The transport simulator met the capacity target. With each simulated channel carrying 24,000 bit/s, the merger reconstructed a synthetic 40,800 bit/s source TS byte for byte using XOR8 (**1.7×**). With XOR16 it did the same at 43,200 bit/s (**1.8×**). These results measure bonding frame transport within the simulation model.

The DVB-T trials used QPSK, 2K, FEC 1/2 and a guard interval of 1/32. The configured source TS bitrate of 50,100 bit/s divided by the per-channel TS bitrate of 30,160 bit/s gives a ratio of **1.66×**. Confirming 1.7× over RF still requires a comparison of usable throughput against a single unbonded 40 kHz channel under the same conditions.

We also decoded the audio lane independently. In a clean IQ recording, channel B yielded 55.680 seconds of continuous audio on its own and 55.168 seconds after merging. After the initial acquisition loss, the remaining encoded audio payload matched byte for byte. Three further recordings from a propagation model allowed us to compare both modes under interrupted reception. [Audio comparison (in Dutch)](../../docs/dvbt-bonding.md#audiovergelijking-kmvkdn--15-november-2026) · [Report on the three reception trials (in Dutch)](../../docs/dvbt-bonding-drie-proeven-20261115.md)

We will add RF spectrum measurements of the two 40 kHz signals at high transmitter power to this report.

## Software integration and tests

The component provides a C API for the transmitter (one source TS in, two channel transport streams out) and the receiver (two channel transport streams in, one TS out). A generator can query the source bitrate, timing requirements and media bitrate budgets in JSON. The receiver application gets an event to start the partner decoder and can read the merger status as JSON. [Public C API](include/dvb_bond.h)

From the repository root, run `make test-bonding-component` to run the five C tests covering packet recovery, partner discovery, TS transport, independent audio and video, and v1 API/ABI compatibility. Run `make test-dvbt-bonding` to also test Linrad DVB integration, including discovery from either channel and preservation of the media payload after merging.

Rob Hardenberg (PE1ITR) developed this experimental protocol. This repository includes component version 1.6.0 under GPL-3.0-only. [Source and revision](ORIGIN) · [Licence](LICENSE)
