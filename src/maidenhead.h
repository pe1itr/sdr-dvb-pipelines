#ifndef MAIDENHEAD_H
#define MAIDENHEAD_H
#include <glib.h>
#include <math.h>
#include <string.h>

/* Coordinates of the cell centre, in degrees. Accept the same 2–8 character
 * locators as My Station; callers trim surrounding whitespace. */
static inline gboolean maidenhead_center(const char *locator, double *latitude, double *longitude) {
    size_t length=locator ? strlen(locator) : 0;
    if (length<2 || length>8 || length%2) return FALSE;
    const int divisions[]={18,10,24,10};
    double lon=-180, lat=-90, width=360, height=180;
    for (size_t i=0;i<length;i+=2) {
        int base=(i/2)%2 ? '0' : 'A';
        int x=g_ascii_toupper(locator[i])-base, y=g_ascii_toupper(locator[i+1])-base;
        int cells=divisions[i/2];
        if (x<0 || x>=cells || y<0 || y>=cells) return FALSE;
        width/=cells; height/=cells;
        lon+=x*width; lat+=y*height;
    }
    if (latitude) *latitude=lat+height/2;
    if (longitude) *longitude=lon+width/2;
    return TRUE;
}

/* Short-path great-circle distance and initial azimuth clockwise from true
 * north on a sphere of mean Earth radius 6371.0088 km. A coincident or
 * antipodal pair has no unique bearing, represented by NAN. */
static inline gboolean maidenhead_bearing(const char *from, const char *to,
        double *distance_km, double *bearing_degrees) {
    double lat1, lon1, lat2, lon2;
    if (!maidenhead_center(from,&lat1,&lon1) || !maidenhead_center(to,&lat2,&lon2)) return FALSE;
    const double radians=G_PI/180;
    lat1*=radians; lat2*=radians;
    double dlon=(lon2-lon1)*radians;
    double a=sin((lat2-lat1)/2), b=sin(dlon/2);
    double hav=CLAMP(a*a+cos(lat1)*cos(lat2)*b*b,0,1);
    *distance_km=6371.0088*2*atan2(sqrt(hav),sqrt(1-hav));
    double east=sin(dlon)*cos(lat2);
    double north=cos(lat1)*sin(lat2)-sin(lat1)*cos(lat2)*cos(dlon);
    *bearing_degrees=hypot(east,north)<1e-12 ? NAN : fmod(atan2(east,north)/radians+360,360);
    return TRUE;
}
#endif
