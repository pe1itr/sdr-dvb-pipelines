#ifndef LINRAD_LEANSDR_BACKEND_H
#define LINRAD_LEANSDR_BACKEND_H
#ifdef __cplusplus
extern "C" {
#endif
int leansdr_backend_main(int argc, char **argv);
void linrad_s2_pls(int modcod, int short_frame, int pilots);
void linrad_fec_result(int good, int corrected);
#ifdef __cplusplus
}
#endif
#endif
