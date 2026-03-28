#!/bin/bash

PROFILE=1
PROFILETEXT1="DVB-S 125k fec 1/2 met u8"
PROFILETEXT2="DVB-S2 125k QPSK FEC 1/2 met u8"
PROFILETEXT3="DVB-S 125k fec 1/2 met s16"
PROFILETEXT4="DVB-S2 125k FEC 1/2 met s16 modcod QPSK FEC 1/2. LDPC bitflips op 100"
PROFILETEXT5="DVB-S2 125k FEC 1/2 met s16 modcod QPSK FEC 1/4, 1/3, 2/5, 1/2, 3/5, 2/3, 3/4 LDPC bitflips op 400"
PROFILETEXT6="DVB-S2 66k FEC 2/3 met s16 modcod QPSK FEC 2/3 LDPC bitflips op 50"
PROFILETEXT7="Meet s16 input levels"
PROFILETEXT8="Meet s16 input SNR voor opgegeven BW en sampelrate"
PROFILETEXT9="Direct van Afedri"
PROFILETEXT10="Direct van Afedri dvb"
PROFILETEXT11="Direct van Afedri dvb"
PROFILETEXT12="Direct van Afedri dvb"
PROFILETEXT13="RTL_SDR DVB-S2 SR 125k"
PROFILETEXT14="HackRF 436 DVB-S2 SR 125k"
PROFILETEXT14="HackRF 437 DVB-S2 SR 333k"
PROFILETEXT16="HackRF PI6EHV DVB-S2 8PSK SR 5M FEC 3/4 - WERKT NIET!!"
PROFILETEXT17="SDR-IQ 436 DVB-S2 SR 125k FEC 1/2"
PROFILETEXT18="SDR-IQ 436 DVB-S2 SR 66k FEC 2/3"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)
      PROFILE="$2"
      shift 2
      ;;
    --info)
      echo "usage option: --profile"
      echo "1: " $PROFILETEXT1
      echo "2: " $PROFILETEXT2
      echo "3: " $PROFILETEXT3
      echo "4: " $PROFILETEXT4
      echo "5: " $PROFILETEXT5
      echo "6: " $PROFILETEXT6
      echo "7: " $PROFILETEXT7
      echo "8: " $PROFILETEXT8
      echo "9: " $PROFILETEXT9
      echo "10: " $PROFILETEXT10
      echo "11: " $PROFILETEXT11
      echo "12: " $PROFILETEXT12
      echo "13: " $PROFILETEXT13
      echo "14: " $PROFILETEXT14
      echo "15: " $PROFILETEXT15
      echo "16: " $PROFILETEXT16
      echo "17: " $PROFILETEXT17
      echo "18: " $PROFILETEXT18
      exit
      ;;
    *)
      echo "Onbekende optie: $1"
      echo "gebruik optie --info om het gebruik te zien"
      exit 1
      ;;
  esac
done

case "$PROFILE" in
  1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18) ;;
  *)
    echo "Ongeldige --profile: $PROFILE"
    exit 1
    ;;
esac


case "$PROFILE" in
  1)
    echo $PROFILETEXT1
    SAMPLERATE=1010526
    SYMBOLRATE=125000
    ./linrad_udp_to_stdout_v2.py --u8 --scale 256 \
    | .././leansdr/src/apps/leandvb -f $SAMPLERATE --standard DVB-S --sr $SYMBOLRATE --cr 1/2 --inbuf 262144 --sampler rrc --rrc-steps 8 --fastlock --hq -v --gui  \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;
  2)
    echo $PROFILETEXT2
    SAMPLERATE=1010526
    SYMBOLRATE=125000
    ./linrad_udp_to_stdout_v2.py --u8 --scale 128 \
    | .././leansdr/src/apps/leandvb -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 20 --modcods 0xFE --framesizes 1 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;
  3)
    echo $PROFILETEXT3
    SAMPLERATE=1010526
    SYMBOLRATE=125000
    ./linrad_udp_to_stdout_v2.py \
    | .././leansdr/src/apps/leandvb --s16 -f $SAMPLERATE --standard DVB-S --sr $SYMBOLRATE --cr 1/2 --inbuf 262144 --sampler rrc --rrc-steps 8 --fastlock --hq -v --gui  \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;
  4)
    echo $PROFILETEXT4
    SAMPLERATE=1010526
    SYMBOLRATE=125000
    ./linrad_udp_to_stdout_v2.py \
    | .././leansdr/src/apps/leandvb --s16 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 100 --modcods 0x10 --framesizes 1 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;
   5)
    echo $PROFILETEXT5
    SAMPLERATE=1010526
    SYMBOLRATE=125000
    ./linrad_udp_to_stdout_v2.py \
    | .././leansdr/src/apps/leandvb --s16 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 400 --modcods 0xFE --framesizes 1 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;
   6)
    echo $PROFILETEXT6
    SAMPLERATE=1010526
    SYMBOLRATE=66000
    ./linrad_udp_to_stdout_v2.py \
    | .././leansdr/src/apps/leandvb --s16 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 50 --modcods 0x40 --framesizes 1 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;
   7)
    echo $PROFILETEXT7
    ./linrad_udp_to_stdout_v2.py --measure 3200000 > /dev/null
    ;;
   8)
    echo $PROFILETEXT8
    ./linrad_udp_to_stdout_v2.py \
    | python3 ./iq_band_snr_meter.py --fs 1010526 --signal-bw 170000 --exclude-bw 300000 --raw-power --avg-blocks 256 --ref-bw 2500
    ;;
   9)
    echo $PROFILETEXT9    
    python3 afedri-udp.py \
    | python3 iq_band_snr_meter.py --fs 1010526 --signal-bw 170000 --exclude-bw 300000 --raw-power --avg-blocks 256 --ref-bw 2500
    ;;
   10)
    echo $PROFILETEXT10
    SAMPLERATE=1010526
    SYMBOLRATE=125000    
    python3 afedri-udp.py \
    | .././leansdr/src/apps/leandvb --s16 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 100 --modcods 0xFE --framesizes 1 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;
   11)
    echo $PROFILETEXT11
    SAMPLERATE=249351
    SYMBOLRATE=125000    
    python3 afedri-udp.py \
    | .././leansdr/src/apps/leandvb --s16 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 100 --modcods 0xFE --framesizes 1 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;
   12)
    echo $PROFILETEXT12
    SAMPLERATE=249351
    SYMBOLRATE=125000    
    python3 afedri-udp.py \
    | .././leansdr/src/apps/leandvb --s16 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 100 --modcods 0xFE --framesizes 1 \
    | tee >(cvlc - --avcodec-hw=none --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3) \
    | tsp -I file - -P analyze --interval 5 -O drop \
    | python3 tsp_monitor.py
    ;;
   13)
    echo "$PROFILETEXT13"
    FREQ=436000000
    SAMPLERATE=2400000
    SYMBOLRATE=125000
    rtl_sdr -f $FREQ -s $SAMPLERATE -g 20 - \
    | .././leansdr/src/apps/leandvb --u8 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 50 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;  
   14)
    echo $PROFILETEXT14
    FREQ=436000000
    SAMPLERATE=2400000
    SYMBOLRATE=125000
    FREQSHIFT=500000
    FREQDIAL=$((FREQ + FREQSHIFT))
    hackrf_transfer -r - -f $FREQDIAL -s $SAMPLERATE -l 16 -g 40 \
    | ./hackrf_s8_to_u8.py \
    | .././leansdr/src/apps/leandvb --u8 -f $SAMPLERATE --tune "$((-FREQSHIFT))" --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 50 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;
   15)
    echo $PROFILETEXT15
    FREQ=437000000
    SAMPLERATE=2400000
    SYMBOLRATE=333000
    FREQSHIFT=500000
    FREQDIAL=$((FREQ + FREQSHIFT))
    hackrf_transfer -r - -f $FREQDIAL -s $SAMPLERATE -l 16 -g 40 \
    | ./hackrf_s8_to_u8.py \
    | .././leansdr/src/apps/leandvb --u8 -f $SAMPLERATE --tune "$((-FREQSHIFT))" --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 50 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;        
   16)
    echo $PROFILETEXT16
    FREQ=1291000000
    SAMPLERATE=10000000
    SYMBOLRATE=5000000
    FREQSHIFT=0
    FREQDIAL=$((FREQ + FREQSHIFT))
    hackrf_transfer -r - -f $FREQDIAL -s $SAMPLERATE -l 16 -g 40 \
    | ./hackrf_s8_to_u8.py \
    | .././leansdr/src/apps/leandvb --u8 -f $SAMPLERATE --tune "$((-FREQSHIFT))" --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 0 --modcods 16384 --framesizes 1 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;
   17)
    echo $PROFILETEXT17
    FREQ=28000000
    SAMPLERATE=196078
    SYMBOLRATE=125000
    ./soapy_sdriq_to_stdout_v2.py -f $FREQ -r $SAMPLERATE -g 10 --u8 --scale 128 \
    | .././leansdr/src/apps/leandvb --u8 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 50 --modcods 0x10 --framesizes 1 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;; 
   18)
    echo $PROFILETEXT18
    FREQ=28000000
    SAMPLERATE=196078
    SYMBOLRATE=66000
    ./soapy_sdriq_to_stdout_v2.py -f $FREQ -r $SAMPLERATE -g 10 --u8 --scale 128 \
    | .././leansdr/src/apps/leandvb --u8 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 10 --modcods 0x40 --framesizes 1 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=300 --live-caching=300 --network-caching=300 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;             
  *)
    echo "default"
    ;;
esac

