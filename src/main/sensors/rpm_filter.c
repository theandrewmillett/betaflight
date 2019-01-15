/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */


#include <math.h>
#include <stdint.h>
#include "platform.h"
#include "build/debug.h"
#include "common/filter.h"
#include "drivers/pwm_output_counts.h"
#include "flight/mixer.h"
#include "flight/pid.h"
#include "pg/pg_ids.h"
#include "sensors/rpm_filter.h"
#include "sensors/gyro.h"

#define RPM_FILTER_MAXHARMONICS 3
#define RPM_MOTOR_FILTER_CUTOFF 150
#define SECONDS_PER_MINUTE      60.0f
#define ERPM_PER_LSB            100.0f
#define MIN_UPDATE_T            0.001f

#if defined(USE_RPM_FILTER)

static pt1Filter_t rpmFilters[MAX_SUPPORTED_MOTORS];

typedef struct rpmNotchFilter_s
{
    uint8_t harmonics;
    uint8_t minHz;
    float   q;
    float   loopTime;

    biquadFilter_t notch[XYZ_AXIS_COUNT][MAX_SUPPORTED_MOTORS][RPM_FILTER_MAXHARMONICS];
} rpmNotchFilter_t;

static float   erpmToHz;
static float   filteredMotorErpm[MAX_SUPPORTED_MOTORS];
static uint8_t numberFilters;
static uint8_t numberRpmNotchFilters;
static uint8_t filterUpdatesPerIteration;
static float   pidLooptime;
static rpmNotchFilter_t filters[2];
static rpmNotchFilter_t* gyroFilter;
static rpmNotchFilter_t* dtermFilter;

PG_REGISTER_WITH_RESET_FN(rpmFilterConfig_t, rpmFilterConfig, PG_RPM_FILTER_CONFIG, 2);

void pgResetFn_rpmFilterConfig(rpmFilterConfig_t *config)
{
    config->gyro_rpm_notch_harmonics = 3;
    config->gyro_rpm_notch_min = 100;
    config->gyro_rpm_notch_q = 500;

    config->dterm_rpm_notch_harmonics = 1;
    config->dterm_rpm_notch_min = 100;
    config->dterm_rpm_notch_q = 500;
}

static void rpmNotchFilterInit(rpmNotchFilter_t* filter, int harmonics, int minHz, int q, float looptime)
{
    filter->harmonics = harmonics;
    filter->minHz = minHz;
    filter->q = q / 100.0f;
    filter->loopTime = looptime;

    for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        for (int motor = 0; motor < getMotorCount(); motor++) {
            for (int i = 0; i < harmonics; i++) {
                biquadFilterInit(
                    &filter->notch[axis][motor][i], minHz * i, looptime, filter->q, FILTER_NOTCH);
            }
        }
    }
}

void rpmFilterInit(const rpmFilterConfig_t *config)
{
    numberRpmNotchFilters = 0;
    if (!motorConfig()->dev.useDshotTelemetry) {
        gyroFilter = dtermFilter = NULL;
        return;
    }

    pidLooptime = gyro.targetLooptime * pidConfig()->pid_process_denom;
    if (config->gyro_rpm_notch_harmonics) {
        gyroFilter = &filters[numberRpmNotchFilters++];
        rpmNotchFilterInit(gyroFilter, config->gyro_rpm_notch_harmonics,
                           config->gyro_rpm_notch_min, config->gyro_rpm_notch_q, gyro.targetLooptime);
    }
    if (config->dterm_rpm_notch_harmonics) {
        dtermFilter = &filters[numberRpmNotchFilters++];
        rpmNotchFilterInit(dtermFilter, config->dterm_rpm_notch_harmonics,
                           config->dterm_rpm_notch_min, config->dterm_rpm_notch_q, pidLooptime);
    }

    for (int i = 0; i < getMotorCount(); i++) {
        pt1FilterInit(&rpmFilters[i], pt1FilterGain(RPM_MOTOR_FILTER_CUTOFF, pidLooptime));
    }

    erpmToHz = ERPM_PER_LSB / SECONDS_PER_MINUTE  / (motorConfig()->motorPoleCount / 2.0f);

    const float loopIterationsPerUpdate = MIN_UPDATE_T / (pidLooptime * 1e-6f);
    numberFilters = getMotorCount() * (filters[0].harmonics + filters[1].harmonics);
    const float filtersPerLoopIteration = numberFilters / loopIterationsPerUpdate;
    filterUpdatesPerIteration = rintf(filtersPerLoopIteration + 0.49f);
}

static float applyFilter(rpmNotchFilter_t* filter, int axis, float value)
{
    if (filter == NULL) {
        return value;
    }
    for (int motor = 0; motor < getMotorCount(); motor++) {
        for (int i = 0; i < filter->harmonics; i++) {
            value = biquadFilterApplyDF1(&filter->notch[axis][motor][i], value);
        }
    }
    return value;
}

float rpmFilterGyro(int axis, float value)
{
    return applyFilter(gyroFilter, axis, value);
}

float rpmFilterDterm(int axis, float value)
{
    return applyFilter(dtermFilter, axis, value);
}

static float motorFrequency[MAX_SUPPORTED_MOTORS];

void rpmFilterUpdate()
{
    if (gyroFilter == NULL && dtermFilter == NULL) {
        return;
    }

    for (int motor = 0; motor < getMotorCount(); motor++) {
        filteredMotorErpm[motor] = pt1FilterApply(&rpmFilters[motor], getDshotTelemetry(motor));
        if (motor < 4) {
            DEBUG_SET(DEBUG_RPM_FILTER, motor, motorFrequency[motor]);
        }
    }

    static uint8_t motor;
    static uint8_t harmonic;
    static uint8_t filter;
    static rpmNotchFilter_t* currentFilter = &filters[0];

    for (int i = 0; i < filterUpdatesPerIteration; i++) {

        float frequency = (harmonic + 1) * motorFrequency[motor];
        const float deactivateFreq = 1000.0f;
        if (frequency < currentFilter->minHz) {
            if (frequency < 0.5f * currentFilter->minHz) {
                frequency = deactivateFreq;
            } else {
                frequency = currentFilter->minHz;
            }
        }
        if (frequency > deactivateFreq) {
            frequency = deactivateFreq;
        }

        biquadFilter_t* template = &currentFilter->notch[0][motor][harmonic];
        // uncomment below to debug filter stepping. Need to also comment out motor rpm DEBUG_SET above
        /* DEBUG_SET(DEBUG_RPM_FILTER, 0, harmonic); */
        /* DEBUG_SET(DEBUG_RPM_FILTER, 1, motor); */
        /* DEBUG_SET(DEBUG_RPM_FILTER, 2, currentFilter == &gyroFilter); */
        /* DEBUG_SET(DEBUG_RPM_FILTER, 3, frequency) */
        biquadFilterUpdate(
            template, frequency, currentFilter->loopTime, currentFilter->q, FILTER_NOTCH);
        for (int axis = 1; axis < XYZ_AXIS_COUNT; axis++) {
            biquadFilter_t* clone = &currentFilter->notch[axis][motor][harmonic];
            clone->b0 = template->b0;
            clone->b1 = template->b1;
            clone->b2 = template->b2;
            clone->a1 = template->a1;
            clone->a2 = template->a2;
        }

        if (++harmonic == currentFilter->harmonics) {
            harmonic = 0;
            if (++filter == numberRpmNotchFilters) {
                filter = 0;
                if (++motor == getMotorCount()) {
                    motor = 0;
                }
                motorFrequency[motor] = erpmToHz * filteredMotorErpm[motor];
            }
            currentFilter = &filters[filter];
        }

    }
}

#endif
