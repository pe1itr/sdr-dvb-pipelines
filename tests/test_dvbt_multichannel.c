/* Actual native mixer + live anti-alias resampler, with BOTH RF channels in
 * the same input. Check sign, streaming phase, passband and adjacent rejection. */
#include "../components/dvbt/src/probe_constellation.c"
#include <assert.h>

static void check_mixer(void)
{
    rbdvbt_iq_mixer_t a, b;
    rbdvbt_complex_t x[17003], y[17003];
    for (size_t i=0; i<17003; ++i) x[i]=(rbdvbt_complex_t){sin(i*.31),cos(i*.71)};
    memcpy(y,x,sizeof(x));
    rbdvbt_iq_mixer_init(&a,192000,0);
    rbdvbt_iq_mixer_apply(&a,x,17003);
    assert(!memcmp(x,y,sizeof(x)));
    rbdvbt_iq_mixer_init(&a,192000,-25000);
    rbdvbt_iq_mixer_init(&b,192000,-25000);
    rbdvbt_iq_mixer_apply(&a,x,17003);
    for (size_t n=0; n<17003;) {
        size_t count=127;
        if (n+count>17003) count=17003-n;
        rbdvbt_iq_mixer_apply(&b,y+n,count);
        n+=count;
    }
    assert(!memcmp(x,y,sizeof(x)));
}

static void check_channels(unsigned rate)
{
    size_t count=rate/10;
    const double output_rate=40000.0*8/7;
    const double tones[]={-25000-16500, -25000+7000, 25000-9000, 25000+16500};
    rbdvbt_complex_t *input=calloc(count,sizeof(*input));
    assert(input);
    for (size_t n=0; n<count; ++n) {
        for (unsigned t=0;t<4;++t) {
            double angle=2*M_PI*tones[t]*n/rate;
            input[n].i+=(float)(.15*cos(angle));
            input[n].q+=(float)(.15*sin(angle));
        }
    }
    for (int channel=0;channel<2;++channel) {
        double offset=channel ? 25000 : -25000;
        rbdvbt_iq_mixer_t mixer;
        rbdvbt_iq_mixer_init(&mixer,rate,offset);
        complexf_t *result=calloc(count,sizeof(*result));
        assert(result);
        size_t total=0;
        live_resampler_release(&live_resampler_state);
        for (size_t n=0;n<count;) {
            size_t take=2048+(n*17+123)%6000;
            if (take+2048>count-n) take=count-n;
            rbdvbt_complex_t *block=malloc(take*sizeof(*block));
            assert(block);
            memcpy(block,input+n,take*sizeof(*block));
            rbdvbt_iq_mixer_apply(&mixer,block,take);
            complexf_t *work=malloc(take*sizeof(*work));
            assert(work);
            for (size_t k=0;k<take;++k) work[k]=(complexf_t){block[k].i,block[k].q};
            complexf_t *out=NULL;
            size_t emitted=0;
            assert(!resample_sinc_ratio_live(work,take,output_rate/rate,rate,
                                             (unsigned)lrint(output_rate),1,&out,&emitted));
            assert(total+emitted<=count);
            memcpy(result+total,out,emitted*sizeof(*out));
            total+=emitted;
            free(out);free(work);free(block);n+=take;
        }
        assert(total>3000);
        for (unsigned t=0;t<4;++t) {
            double re=0,im=0;
            double frequency=tones[t]-offset;
            size_t begin=500, end=total-500;
            for (size_t n=begin;n<end;++n) {
                double angle=-2*M_PI*frequency*n/output_rate;
                re+=result[n].re*cos(angle)-result[n].im*sin(angle);
                im+=result[n].re*sin(angle)+result[n].im*cos(angle);
            }
            double amplitude=hypot(re,im)/(end-begin);
            int wanted=(channel==0 ? t<2 : t>=2);
            fprintf(stderr,"rate=%u channel=%+.0f tone=%.0f amplitude=%.6f wanted=%d\n",
                    rate,offset,tones[t],amplitude,wanted);
            if (wanted) assert(amplitude>.14 && amplitude<.16);
            else assert(amplitude<.0015); /* at least 40 dB adjacent-tone rejection */
        }
        free(result);
    }
    live_resampler_release(&live_resampler_state);
    free(input);
}

int main(void)
{
    rbdvbt_log_set_level(RBDVBT_LOG_ERROR);
    check_mixer();
    check_channels(192000);  /* FUNcube Pro+ */
    check_channels(2400000); /* RTL-SDR */
    puts("DVB-T multichannel: streaming mixer and adjacent-channel rejection passed");
    return 0;
}
