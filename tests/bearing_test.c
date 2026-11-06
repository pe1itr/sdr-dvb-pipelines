#include "../src/maidenhead.h"

int main(void) {
    double lat, lon, km, bearing, reverse_km, reverse_bearing;
    g_assert_true(maidenhead_center("JO",&lat,&lon));
    g_assert_cmpfloat_with_epsilon(lat,55,1e-10);
    g_assert_cmpfloat_with_epsilon(lon,10,1e-10);
    g_assert_true(maidenhead_center("JO21",&lat,&lon));
    g_assert_cmpfloat_with_epsilon(lat,51.5,1e-10);
    g_assert_cmpfloat_with_epsilon(lon,5,1e-10);
    g_assert_true(maidenhead_center("jo21qk",&lat,&lon));
    g_assert_cmpfloat_with_epsilon(lat,51.4375,1e-10);
    g_assert_cmpfloat_with_epsilon(lon,5.375,1e-10);
    g_assert_true(maidenhead_center("JO21QK12",&lat,&lon));
    g_assert_cmpfloat_with_epsilon(lat,51.427083333333,1e-10);
    g_assert_cmpfloat_with_epsilon(lon,5.345833333333,1e-10);
    const char *invalid[]={"", "J", "JO2", "SS", "JOAA", "JO21YY", "JO21QK1A", "JO21QK1234", "éé"};
    for (unsigned i=0;i<G_N_ELEMENTS(invalid);++i) {
        g_assert_false(maidenhead_center(invalid[i],NULL,NULL));
        g_assert_false(maidenhead_bearing("JO21QK",invalid[i],&km,&bearing));
        g_assert_false(maidenhead_bearing(invalid[i],"JO21QK",&km,&bearing));
    }
    /* A one-degree meridian arc has a known length and cardinal bearing. */
    g_assert_true(maidenhead_bearing("JO21QK","JO22QK",&km,&bearing));
    g_assert_cmpfloat_with_epsilon(km,111.1950802335,1e-7);
    g_assert_cmpfloat_with_epsilon(bearing,0,1e-10);
    g_assert_true(maidenhead_bearing("JO22QK","JO21QK",&reverse_km,&reverse_bearing));
    g_assert_cmpfloat_with_epsilon(reverse_km,km,1e-10);
    g_assert_cmpfloat_with_epsilon(reverse_bearing,180,1e-10);
    /* Cross the date line on the short path; reverse direction is westward. */
    g_assert_true(maidenhead_bearing("RJ","AJ",&km,&bearing));
    g_assert_cmpfloat(km,>,2200); g_assert_cmpfloat(km,<,2230);
    g_assert_cmpfloat(bearing,>,89); g_assert_cmpfloat(bearing,<,90);
    g_assert_true(maidenhead_bearing("AJ","RJ",&reverse_km,&reverse_bearing));
    g_assert_cmpfloat_with_epsilon(reverse_km,km,1e-9);
    g_assert_cmpfloat(reverse_bearing,>,270); g_assert_cmpfloat(reverse_bearing,<,271);
    g_assert_true(maidenhead_bearing("JO21QK","jo21qk",&km,&bearing));
    g_assert_cmpfloat(km,==,0); g_assert_true(isnan(bearing));
    g_assert_true(maidenhead_bearing("JO","AD",&km,&bearing));
    g_assert_cmpfloat_with_epsilon(km,20015.114442,0.001);
    g_assert_true(isnan(bearing));
    g_print("Bearing: locator conversion, validation, distance and direction checks passed.\n");
    return 0;
}
