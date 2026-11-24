#include "moon.h"
#include "../../third_party/astronomy/astronomy.h"
#include "../../src/maidenhead.h"
#include <stdio.h>

/* Instantaneous geometric range rate of the lunar centre relative to each
 * rotating terrestrial station, in the same J2000 inertial frame. */
static int moon_leg(astro_time_t time, astro_state_vector_t moon, MoonSite site,
                    double tx, MoonLeg *leg)
{
    astro_observer_t observer = Astronomy_MakeObserver(site.latitude, site.longitude, 0);
    astro_state_vector_t station = Astronomy_ObserverState(&time, observer, EQUATOR_J2000);
    double x = moon.x - station.x, y = moon.y - station.y, z = moon.z - station.z;
    double range = hypot(hypot(x, y), z);
    double speed = (x*(moon.vx-station.vx) + y*(moon.vy-station.vy) + z*(moon.vz-station.vz))/range;
    astro_equatorial_t eq = Astronomy_Equator(BODY_MOON, &time, observer, EQUATOR_OF_DATE, NO_ABERRATION);
    if (station.status || eq.status) return 0;
    astro_horizon_t hor = Astronomy_Horizon(&time, observer, eq.ra, eq.dec, REFRACTION_NONE);
    *leg = (MoonLeg){hor.azimuth, hor.altitude, -tx * speed / C_AUDAY, range * KM_PER_AU};
    return isfinite(leg->doppler) && isfinite(hor.altitude);
}

int moon_predict(double utc, double tx, const MoonSite *rx, const MoonSite *dx, MoonPrediction *p)
{
    if (!isfinite(utc) || !isfinite(tx) || tx < 0) return 0;
    astro_time_t time = Astronomy_TimeFromDays((utc - 946728000.0)/86400.0);
    astro_state_vector_t moon = Astronomy_GeoMoonState(time);
    p->rx = p->dx = (MoonLeg){NAN,NAN,NAN,NAN};
    if (moon.status || (rx && !moon_leg(time, moon, *rx, tx, &p->rx)) ||
        (dx && !moon_leg(time, moon, *dx, tx, &p->dx))) return 0;
    p->doppler = p->rx.doppler + p->dx.doppler;
    return 1;
}

int moon_tracker_parse(MoonTracker *m, const char *spec)
{
    if (!spec) return 1;
    char **v = g_strsplit(spec, ",", -1);
    int ok = g_strv_length(v) == 5 &&
        maidenhead_center(v[0], &m->rx.latitude, &m->rx.longitude) &&
        maidenhead_center(v[1], &m->dx.latitude, &m->dx.longitude);
    double *fields[] = {&m->tx, &m->center, &m->width};
    for (unsigned i=0; ok && i<3; ++i) {
        char *end;
        *fields[i] = g_ascii_strtod(v[i+2], &end);
        ok = !*end && isfinite(*fields[i]) && *fields[i]>0;
    }
    g_strfreev(v);
    if (!ok) fprintf(stderr,"Invalid --moon-track stations, RF frequencies or channel width\n");
    m->next_update = -INFINITY;
    return ok;
}

/* Called at acquisition, before the output FIFO, or with the file sample
 * clock. Only the phase increment changes at a prediction boundary. */
int moon_tracker_update(MoonTracker *m, IQShift *shift, double rate, double utc)
{
    if (utc < m->next_update && utc >= m->next_update-1.0) return 1;
    MoonPrediction p;
    if (!moon_predict(utc, m->tx, &m->rx, &m->dx, &p)) return 0;
    double offset = (m->tx - m->center) + p.doppler;
    if (!iq_shift_fits(rate, offset, m->width)) {
        fprintf(stderr,"[moon] Predicted echo and channel width exceed the IQ band\n");
        return 0;
    }
    double angle = -2*G_PI*offset/rate;
    shift->offset = offset;
    shift->step_re = cos(angle);
    shift->step_im = sin(angle);
    m->next_update = utc + 1;
    fprintf(stderr,"[moon] sample_utc=%.6f doppler=%+.3f offset=%+.3f\n",utc,p.doppler,offset);
    return 1;
}
