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
PROFILETEXT9="Direct van Afedri SNR measurement of 125 ks (170khz BW) signal"
PROFILETEXT10="Direct van Afedri DVB-S2 125k "
PROFILETEXT11="Direct van Afedri DVB-S2 125k (low sdr samplerate)"
PROFILETEXT12="Direct van Afedri DVB-S2 125k (low sdr samplerate) with TS info test"
PROFILETEXT13="RTL_SDR DVB-S2 SR 125k"
PROFILETEXT14="HackRF 436 DVB-S2 SR 125k"
PROFILETEXT15="HackRF 437 DVB-S2 SR 333k"
PROFILETEXT16="HackRF PI6EHV DVB-S2 8PSK SR 5M FEC 3/4 - WERKT NIET!!"
PROFILETEXT17="SDR-IQ 436 DVB-S2 SR 125k FEC 1/2"
PROFILETEXT18="SDR-IQ 436 DVB-S2 SR 66k FEC 2/3"
PROFILETEXT19="SDR-IQ 436 DVB-S2 SR 35k FEC 2/3 (very low sdr samplerate)"
PROFILETEXT20="DVB-T SR 333k FEC 2/3 via linrad"
PROFILETEXT21="DVB-T SR 333k FEC 2/3 via linrad and recording"
PROFILETEXT22="DVB-T SR 250k FEC 2/3 via RTL_SDR"
PROFILETEXT23="RTL SDR TSscatterfix"
PROFILETEXT24="Linrad Afedri TSscatterfix 436 DVB-S SR 125k FEC 1/2"
PROFILETEXT25="Linrad Afedri TSscatterfix 436 DVB-S2 SR 125k FEC 1/2"
PROFILETEXT25="6m"

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
      echo "19: " $PROFILETEXT19
      echo "20: " $PROFILETEXT20
      echo "21: " $PROFILETEXT21
      echo "22: " $PROFILETEXT22
      echo "23: " $PROFILETEXT23
      echo "24: " $PROFILETEXT24
      echo "25: " $PROFILETEXT25
      echo "26: " $PROFILETEXT26
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
  1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|26) ;;
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
    | .././leansdr/src/apps/leandvb --s16 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 50 --modcods 0x10 --framesizes 1 \
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
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=600 --live-caching=600 --network-caching=600 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;   
   19)
    echo $PROFILETEXT19
    FREQ=28000000
    SAMPLERATE=111111
    SYMBOLRATE=35000
    ./soapy_sdriq_to_stdout_v2.py -f $FREQ -r $SAMPLERATE -g 10 --u8 --scale 128 \
    | .././leansdr/src/apps/leandvb --u8 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 3 --modcods 0x40 --framesizes 1 \
    | cvlc --avcodec-hw=none fd://0 --demux=ts --file-caching=1500 --live-caching=1500 --network-caching=1500 --clock-jitter=0 --clock-synchro=0 --drop-late-frames --skip-frames --zoom=3
    ;;
   20)
    echo $PROFILETEXT20
    SAMPLERATE=1010526
    SYMBOLRATE=250000
    LIVESYMBOLS=64
    SESSION=$(date +%Y%m%d_%H%M%S)
    RXLOG="rx_${SESSION}.log"
    ./linrad_udp_to_stdout_v2.py \
    | .././dvbt-rx/build/rbdvbt_rx --stdin --live --input-format s16 --sample-rate $SAMPLERATE --afc --sr $SYMBOLRATE --gi 1/32 --fec 1/2 --resample-to-dvbt-rate \
    --live-symbols $LIVESYMBOLS --probe-symbols $LIVESYMBOLS --stdout-ts --wait-video-start --status-json /tmp/rx_status.json  --loglevel info --gui 2> "$RXLOG" \
    | ffplay -f mpegts -loglevel error -
    ;;
   21)
    echo $PROFILETEXT21
    SAMPLERATE=1010526
    SYMBOLRATE=150000
    LIVESYMBOLS=128
    SESSION=$(date +%Y%m%d_%H%M%S)
    RXLOG="rx_${SESSION}.log"
    ./linrad_udp_to_stdout_v3.py --record-iq --record-stdout \
    | .././dvbt-rx/build/rbdvbt_rx --stdin --live --input-format s16 --sample-rate $SAMPLERATE --afc --sr $SYMBOLRATE --gi 1/32 --fec 1/2 --resample-to-dvbt-rate \
    --live-symbols $LIVESYMBOLS --probe-symbols $LIVESYMBOLS --stdout-ts --wait-video-start --status-json /tmp/rx_status.json  --loglevel info --gui 2> "$RXLOG" \
    | ffplay -f mpegts -loglevel error -
    ;;
   22)
    echo $PROFILETEXT22
    SAMPLERATE=1010526
    SYMBOLRATE=150000
    SESSION=$(date +%Y%m%d_%H%M%S)
    RXLOG="rx_${SESSION}.log"
    rtl_sdr -f 437000000 -s $SAMPLERATE -g 0 - \
    | .././dvbt-rx/build/rbdvbt_rx --stdin --live --input-format u8 --sample-rate $SAMPLERATE --sr $SYMBOLRATE --gi 1/32 --fec 1/2 --resample-to-dvbt-rate \
    --live-symbols 64 --probe-symbols 64 --stdout-ts --wait-video-start --udp-out 127.0.0.1:10000 --status-json /tmp/rx_status.json  --loglevel info --gui 2> "$RXLOG" \
    | ffplay -f mpegts -loglevel error -
    ;;
   23)
    echo "$PROFILETEXT23"
    FREQ=436000000
    SAMPLERATE=2400000
    SYMBOLRATE=125000
    rtl_sdr -f $FREQ -s $SAMPLERATE -g 20 - \
    | .././leansdr/src/apps/leandvb --u8 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 50 \
    | ../TSscatterfix/tools/contest_live.py \
      --work-dir ../TSscatterfix/temp/live \
      --model ../TSscatterfix/temp/linrad_fragments_5way_conf/model_combined.json \
      --udp-output 127.0.0.1:1235 \
      --output-fps 10 \
      --output-size 640x360 \
      --startup-interval 0.7 \
      --interval 1 \
      --startup-analysis-packets 50 \
      --analysis-packets 150 \
      --min-packets 150
    ;;
   24)
    echo "$PROFILETEXT24"
    FREQ=436000000
    SAMPLERATE=1010526
    SYMBOLRATE=125000
    ./linrad_udp_to_stdout_v2.py \
    | .././leansdr/src/apps/leandvb --s16 -f $SAMPLERATE --standard DVB-S --sr $SYMBOLRATE --cr 1/2 --inbuf 262144 --sampler rrc --rrc-steps 8 --fastlock --hq -v --gui  \
    | ../TSscatterfix/tools/contest_live.py \
      --work-dir ../TSscatterfix/temp/live \
      --model ../TSscatterfix/temp/linrad_fragments_5way_conf/model_combined.json \
      --udp-output 127.0.0.1:1235 \
      --output-fps 1 \
      --codec auto \
      --output-size 640x360 \
      --startup-interval 0.7 \
      --interval 1 \
      --startup-analysis-packets 20 \
      --analysis-packets 150 \
      --min-packets 20
    ;;
   25)
    echo "$PROFILETEXT25"
    FREQ=436000000
    SAMPLERATE=1010526
    SYMBOLRATE=125000
    ./linrad_udp_to_stdout_v3.py \
    | .././leansdr/src/apps/leandvb --s16 -f $SAMPLERATE --standard DVB-S2 --sr $SYMBOLRATE --gui --fastlock --sampler rrc --rrc-steps 8 --ldpc-bf 20 --modcods 0x10 --framesizes 1 \
    | ../TSscatterfix/tools/contest_live.py \
      --work-dir ../TSscatterfix/temp/live \
      --model ../TSscatterfix/temp/linrad_fragments_5way_conf/model_combined.json \
      --udp-output 127.0.0.1:1235 \
      --output-fps 1 \
      --output-size 640x360 \
      --startup-interval 0.7 \
      --interval 1 \
      --startup-analysis-packets 20 \
      --analysis-packets 150 \
      --min-packets 20
    ;;
   26)
    echo "$PROFILETEXT26"
    FREQ=145446000
    SAMPLERATE=2048000
    SYMBOLRATE=125000
    rtl_sdr -f $FREQ -s $SAMPLERATE -g 30 - \
    | .././leansdr/src/apps/leandvb --u8 -f $SAMPLERATE --standard DVB-S --sr $SYMBOLRATE --cr 1/2 --inbuf 262144 --sampler rrc --rrc-steps 8 --fastlock --hq -v --gui \
    | ../TSscatterfix/tools/contest_live.py \
      --work-dir ../TSscatterfix/temp/live \
      --model ../TSscatterfix/temp/linrad_fragments_5way_conf/model_combined.json \
      --udp-output 127.0.0.1:1235 \
      --output-fps 1 \
      --codec auto \
      --output-size 640x360 \
      --startup-interval 0.7 \
      --interval 1 \
      --startup-analysis-packets 20 \
      --analysis-packets 150 \
      --min-packets 20
    ;;
   *)
    echo "default"
    ;;
esac

