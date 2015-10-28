// adapted from frameworks/native/services/sensorservice/Fusion.cpp

#include <fusion/fusion.h>

#include <fusion/mat.h>

#include <errno.h>
#include <nanohub_math.h>
#include <stdio.h>

#include <seos.h>

#ifdef DEBUG_CH
// change to 0 to disable fusion debugging output
#define DEBUG_FUSION  0
#endif

#define ACC     1
#define MAG     2
#define GYRO    4

#define DEFAULT_GYRO_VAR         1e-7
#define DEFAULT_GYRO_BIAS_VAR    1e-12
#define DEFAULT_ACC_STDEV        1.5e-2f
#define DEFAULT_MAG_STDEV        1.0e-2f

#define GEOMAG_GYRO_VAR          1e-4
#define GEOMAG_GYRO_BIAS_VAR     1e-8
#define GEOMAG_ACC_STDEV         0.05f
#define GEOMAG_MAG_STDEV         0.1f

#define SYMMETRY_TOLERANCE       1e-10f
#define FAKE_MAG_INTERVAL        1.0f  //sec

#define NOMINAL_GRAVITY          9.81f
#define FREE_FALL_THRESHOLD      (0.1f * NOMINAL_GRAVITY)
#define FREE_FALL_THRESHOLD_SQ   (FREE_FALL_THRESHOLD * FREE_FALL_THRESHOLD)

#define MAX_VALID_MAGNETIC_FIELD    100.0f
#define MAX_VALID_MAGNETIC_FIELD_SQ (MAX_VALID_MAGNETIC_FIELD * MAX_VALID_MAGNETIC_FIELD)

#define MIN_VALID_MAGNETIC_FIELD    10.0f
#define MIN_VALID_MAGNETIC_FIELD_SQ (MIN_VALID_MAGNETIC_FIELD * MIN_VALID_MAGNETIC_FIELD)

#define MIN_VALID_CROSS_PRODUCT_MAG     1.0e-3
#define MIN_VALID_CROSS_PRODUCT_MAG_SQ  (MIN_VALID_CROSS_PRODUCT_MAG * MIN_VALID_CROSS_PRODUCT_MAG)

void initFusion(struct Fusion *fusion, uint32_t flags) {
    fusion->flags = flags;

    if (flags & FUSION_USE_GYRO) {
        // normal fusion mode
        fusion->param.gyro_var = DEFAULT_GYRO_VAR;
        fusion->param.gyro_bias_var = DEFAULT_GYRO_BIAS_VAR;
        fusion->param.acc_stdev = DEFAULT_ACC_STDEV;
        fusion->param.mag_stdev = DEFAULT_MAG_STDEV;
    } else {
        // geo mag mode
        fusion->param.gyro_var = GEOMAG_GYRO_VAR;
        fusion->param.gyro_bias_var = GEOMAG_GYRO_BIAS_VAR;
        fusion->param.acc_stdev = GEOMAG_ACC_STDEV;
        fusion->param.mag_stdev = GEOMAG_MAG_STDEV;
    }

    if (flags & FUSION_REINITIALIZE)
    {
        initVec3(&fusion->Ba, 0.0f, 0.0f, 1.0f);
        initVec3(&fusion->Bm, 0.0f, 1.0f, 0.0f);

        initVec4(&fusion->x0, 0.0f, 0.0f, 0.0f, 0.0f);
        initVec3(&fusion->x1, 0.0f, 0.0f, 0.0f);

        fusion->mInitState = 0;

        fusion->mGyroRate = 0.0f;
        fusion->mCount[0] = fusion->mCount[1] = fusion->mCount[2] = 0;

        initVec3(&fusion->mData[0], 0.0f, 0.0f, 0.0f);
        initVec3(&fusion->mData[1], 0.0f, 0.0f, 0.0f);
        initVec3(&fusion->mData[2], 0.0f, 0.0f, 0.0f);
    } else  {
        // mask off disabled sensor bit
        fusion->mInitState &= (ACC
                               | ((fusion->flags & FUSION_USE_MAG) ? MAG : 0)
                               | ((fusion->flags & FUSION_USE_GYRO) ? GYRO : 0));
    }
}

int fusionHasEstimate(const struct Fusion *fusion) {
    // waive sensor init depends on the mode
    return fusion->mInitState == (ACC
                                  | ((fusion->flags & FUSION_USE_MAG) ? MAG : 0)
                                  | ((fusion->flags & FUSION_USE_GYRO) ? GYRO : 0));
}

static void internalInit(struct Fusion *fusion, const Quat *q, float dT) {
    fusion->x0 = *q;
    initVec3(&fusion->x1, 0.0f, 0.0f, 0.0f);

    float dT2 = dT * dT;
    float dT3 = dT2 * dT;

    float q00 = fusion->param.gyro_var * dT +
                0.33333f * fusion->param.gyro_bias_var * dT3;
    float q11 = fusion->param.gyro_var * dT;
    float q10 = 0.5f * fusion->param.gyro_bias_var * dT2;
    float q01 = q10;

    initDiagonalMatrix(&fusion->GQGt[0][0], q00);
    initDiagonalMatrix(&fusion->GQGt[0][1], -q10);
    initDiagonalMatrix(&fusion->GQGt[1][0], -q01);
    initDiagonalMatrix(&fusion->GQGt[1][1], q11);

    initZeroMatrix(&fusion->P[0][0]);
    initZeroMatrix(&fusion->P[0][1]);
    initZeroMatrix(&fusion->P[1][0]);
    initZeroMatrix(&fusion->P[1][1]);
}

static int fusion_init_complete(struct Fusion *fusion, int what, const struct Vec3 *d, float dT) {
    if (fusionHasEstimate(fusion)) {
        return 1;
    }

    switch (what) {
        case ACC:
        {
            if (!(fusion->flags & FUSION_USE_GYRO)) {
                fusion->mGyroRate = dT;
            }
            struct Vec3 unityD = *d;
            vec3Normalize(&unityD);

            vec3Add(&fusion->mData[0], &unityD);
            ++fusion->mCount[0];

            if (fusion->mCount[0] == 32) {
                fusion->mInitState |= ACC;
            }
            break;
        }

        case MAG:
        {
            struct Vec3 unityD = *d;
            vec3Normalize(&unityD);

            vec3Add(&fusion->mData[1], &unityD);
            ++fusion->mCount[1];

            fusion->mInitState |= MAG;
            break;
        }

        case GYRO:
        {
            fusion->mGyroRate = dT;

            struct Vec3 scaledD = *d;
            vec3ScalarMul(&scaledD, dT);

            vec3Add(&fusion->mData[2], &scaledD);
            ++fusion->mCount[2];

            fusion->mInitState |= GYRO;
            break;
        }

        default:
            // assert(!"should not be here");
            break;
    }

    if (fusionHasEstimate(fusion)) {
        vec3ScalarMul(&fusion->mData[0], 1.0f / fusion->mCount[0]);

        if (fusion->flags & FUSION_USE_MAG) {
            vec3ScalarMul(&fusion->mData[1], 1.0f / fusion->mCount[1]);
        }

        struct Vec3 up = fusion->mData[0];

        struct Vec3 east;
        if (fusion->flags & FUSION_USE_MAG) {
            vec3Cross(&east, &fusion->mData[1], &up);
            vec3Normalize(&east);
        } else {
            findOrthogonalVector(up.x, up.y, up.z, &east.x, &east.y, &east.z);
        }

        struct Vec3 north;
        vec3Cross(&north, &up, &east);

        struct Mat33 R;
        initMatrixColumns(&R, &east, &north, &up);

        Quat q;
        initQuat(&q, &R);

        internalInit(fusion, &q, fusion->mGyroRate);
    }

    return 0;
}

static void matrixCross(struct Mat33 *out, struct Vec3 *p, float diag) {
    out->elem[0][0] = diag;
    out->elem[1][1] = diag;
    out->elem[2][2] = diag;
    out->elem[1][0] = p->z;
    out->elem[0][1] = -p->z;
    out->elem[2][0] = -p->y;
    out->elem[0][2] = p->y;
    out->elem[2][1] = p->x;
    out->elem[1][2] = -p->x;
}

static void fusionCheckState(struct Fusion *fusion) {

    if (!mat33IsPositiveSemidefinite(&fusion->P[0][0], SYMMETRY_TOLERANCE)
            || !mat33IsPositiveSemidefinite(
                &fusion->P[1][1], SYMMETRY_TOLERANCE)) {

        initZeroMatrix(&fusion->P[0][0]);
        initZeroMatrix(&fusion->P[0][1]);
        initZeroMatrix(&fusion->P[1][0]);
        initZeroMatrix(&fusion->P[1][1]);
    }
}

#define kEps 1.0E-4f

static void fusionPredict(struct Fusion *fusion, const struct Vec3 *w, float dT) {
    Quat q = fusion->x0;
    struct Vec3 b = fusion->x1;

    struct Vec3 we = *w;
    vec3Sub(&we, &b);

    struct Mat33 I33;
    initDiagonalMatrix(&I33, 1.0f);

    struct Mat33 I33dT;
    initDiagonalMatrix(&I33dT, dT);

    struct Mat33 wx;
    matrixCross(&wx, &we, 0.0f);

    struct Mat33 wx2;
    mat33Multiply(&wx2, &wx, &wx);

    float norm_we = vec3Norm(&we);

    if (fabsf(norm_we) < kEps) {
        return;
    }

    float lwedT = norm_we * dT;
    float hlwedT = 0.5f * lwedT;
    float ilwe = 1.0f / norm_we;
    float k0 = (1.0f - cosf(lwedT)) * (ilwe * ilwe);
    float k1 = sinf(lwedT);
    float k2 = cosf(hlwedT);

    struct Vec3 psi = we;
    vec3ScalarMul(&psi, sinf(hlwedT) * ilwe);

    struct Vec3 negPsi = psi;
    vec3ScalarMul(&negPsi, -1.0f);

    struct Mat33 O33;
    matrixCross(&O33, &negPsi, k2);

    struct Mat44 O;
    size_t i;
    for (i = 0; i < 3; ++i) {
        size_t j;
        for (j = 0; j < 3; ++j) {
            O.elem[i][j] = O33.elem[i][j];
        }
    }

    O.elem[3][0] = -psi.x;
    O.elem[3][1] = -psi.y;
    O.elem[3][2] = -psi.z;
    O.elem[3][3] = k2;

    O.elem[0][3] = psi.x;
    O.elem[1][3] = psi.y;
    O.elem[2][3] = psi.z;

    struct Mat33 tmp = wx;
    mat33ScalarMul(&tmp, k1 * ilwe);

    fusion->Phi0[0] = I33;
    mat33Sub(&fusion->Phi0[0], &tmp);

    tmp = wx2;
    mat33ScalarMul(&tmp, k0);

    mat33Add(&fusion->Phi0[0], &tmp);

    tmp = wx;
    mat33ScalarMul(&tmp, k0);
    fusion->Phi0[1] = tmp;

    mat33Sub(&fusion->Phi0[1], &I33dT);

    tmp = wx2;
    mat33ScalarMul(&tmp, ilwe * ilwe * ilwe * (lwedT - k1));

    mat33Sub(&fusion->Phi0[1], &tmp);

    mat44Apply(&fusion->x0, &O, &q);

    if (fusion->x0.w < 0.0f) {
        fusion->x0.x = -fusion->x0.x;
        fusion->x0.y = -fusion->x0.y;
        fusion->x0.z = -fusion->x0.z;
        fusion->x0.w = -fusion->x0.w;
    }

    // Pnew = Phi * P

    struct Mat33 Pnew[2][2];
    mat33Multiply(&Pnew[0][0], &fusion->Phi0[0], &fusion->P[0][0]);
    mat33Multiply(&tmp, &fusion->Phi0[1], &fusion->P[1][0]);
    mat33Add(&Pnew[0][0], &tmp);

    mat33Multiply(&Pnew[0][1], &fusion->Phi0[0], &fusion->P[0][1]);
    mat33Multiply(&tmp, &fusion->Phi0[1], &fusion->P[1][1]);
    mat33Add(&Pnew[0][1], &tmp);

    Pnew[1][0] = fusion->P[1][0];
    Pnew[1][1] = fusion->P[1][1];

    // P = Pnew * Phi^T

    mat33MultiplyTransposed2(&fusion->P[0][0], &Pnew[0][0], &fusion->Phi0[0]);
    mat33MultiplyTransposed2(&tmp, &Pnew[0][1], &fusion->Phi0[1]);
    mat33Add(&fusion->P[0][0], &tmp);

    fusion->P[0][1] = Pnew[0][1];

    mat33MultiplyTransposed2(&fusion->P[1][0], &Pnew[1][0], &fusion->Phi0[0]);
    mat33MultiplyTransposed2(&tmp, &Pnew[1][1], &fusion->Phi0[1]);
    mat33Add(&fusion->P[1][0], &tmp);

    fusion->P[1][1] = Pnew[1][1];

    mat33Add(&fusion->P[0][0], &fusion->GQGt[0][0]);
    mat33Add(&fusion->P[0][1], &fusion->GQGt[0][1]);
    mat33Add(&fusion->P[1][0], &fusion->GQGt[1][0]);
    mat33Add(&fusion->P[1][1], &fusion->GQGt[1][1]);

    fusionCheckState(fusion);
}

void fusionHandleGyro(struct Fusion *fusion, const struct Vec3 *w, float dT) {
    if (!fusion_init_complete(fusion, GYRO, w, dT)) {
        return;
    }

    fusionPredict(fusion, w, dT);
}

static void scaleCovariance(struct Mat33 *out, const struct Mat33 *A, const struct Mat33 *P) {
    size_t r;
    for (r = 0; r < 3; ++r) {
        size_t j;
        for (j = r; j < 3; ++j) {
            float apat = 0.0f;
            size_t c;
            for (c = 0; c < 3; ++c) {
                float v = A->elem[c][r] * P->elem[c][c] * 0.5f;
                size_t k;
                for (k = c + 1; k < 3; ++k) {
                    v += A->elem[k][r] * P->elem[c][k];
                }

                apat += 2.0f * v * A->elem[c][j];
            }

            out->elem[r][j] = apat;
            out->elem[j][r] = apat;
        }
    }
}

static void getF(struct Vec4 F[3], const struct Vec4 *q) {
    F[0].x = q->w;      F[1].x = -q->z;         F[2].x = q->y;
    F[0].y = q->z;      F[1].y = q->w;          F[2].y = -q->x;
    F[0].z = -q->y;     F[1].z = q->x;          F[2].z = q->w;
    F[0].w = -q->x;     F[1].w = -q->y;         F[2].w = -q->z;
}

static void fusionUpdate(
        struct Fusion *fusion, const struct Vec3 *z, const struct Vec3 *Bi, float sigma) {
    struct Mat33 A;
    quatToMatrix(&A, &fusion->x0);

    struct Vec3 Bb;
    mat33Apply(&Bb, &A, Bi);

    struct Mat33 L;
    matrixCross(&L, &Bb, 0.0f);

    struct Mat33 R;
    initDiagonalMatrix(&R, sigma * sigma);

    struct Mat33 S;
    scaleCovariance(&S, &L, &fusion->P[0][0]);

    mat33Add(&S, &R);

    struct Mat33 Si;
    mat33Invert(&Si, &S);

    struct Mat33 LtSi;
    mat33MultiplyTransposed(&LtSi, &L, &Si);

    struct Mat33 K[2];
    mat33Multiply(&K[0], &fusion->P[0][0], &LtSi);
    mat33MultiplyTransposed(&K[1], &fusion->P[0][1], &LtSi);

    struct Mat33 K0L;
    mat33Multiply(&K0L, &K[0], &L);

    struct Mat33 K1L;
    mat33Multiply(&K1L, &K[1], &L);

    struct Mat33 tmp;
    mat33Multiply(&tmp, &K0L, &fusion->P[0][0]);
    mat33Sub(&fusion->P[0][0], &tmp);

    mat33Multiply(&tmp, &K1L, &fusion->P[0][1]);
    mat33Sub(&fusion->P[1][1], &tmp);

    mat33Multiply(&tmp, &K0L, &fusion->P[0][1]);
    mat33Sub(&fusion->P[0][1], &tmp);

    mat33Transpose(&fusion->P[1][0], &fusion->P[0][1]);

    struct Vec3 e = *z;
    vec3Sub(&e, &Bb);

    struct Vec3 dq;
    mat33Apply(&dq, &K[0], &e);


    struct Vec4 F[3];
    getF(F, &fusion->x0);

    // 4x3 * 3x1 => 4x1

    struct Vec4 q;
    q.x = fusion->x0.x + 0.5f * (F[0].x * dq.x + F[1].x * dq.y + F[2].x * dq.z);
    q.y = fusion->x0.y + 0.5f * (F[0].y * dq.x + F[1].y * dq.y + F[2].y * dq.z);
    q.z = fusion->x0.z + 0.5f * (F[0].z * dq.x + F[1].z * dq.y + F[2].z * dq.z);
    q.w = fusion->x0.w + 0.5f * (F[0].w * dq.x + F[1].w * dq.y + F[2].w * dq.z);

    fusion->x0 = q;
    quatNormalize(&fusion->x0);

    if (fusion->flags & FUSION_USE_MAG) {
        // accumulate gyro bias (causes self spin) only if not
        // game rotation vector
        struct Vec3 db;
        mat33Apply(&db, &K[1], &e);
        vec3Add(&fusion->x1, &db);
    }

    fusionCheckState(fusion);
}

int fusionHandleAcc(struct Fusion *fusion, const struct Vec3 *a, float dT) {
    if (!fusion_init_complete(fusion, ACC, a,  dT)) {
        return -EINVAL;
    }

    float norm2 = vec3NormSquared(a);

    if (norm2 < FREE_FALL_THRESHOLD_SQ) {
        return -EINVAL;
    }

    float l = sqrtf(norm2);
    float l_inv = 1.0f / l;

    if (!(fusion->flags & FUSION_USE_GYRO)) {
        // geo mag mode
        // drive the Kalman filter with zero mean dummy gyro vector
        struct Vec3 w_dummy;

        // avoid (fabsf(norm_we) < kEps) in fusionPredict()
        initVec3(&w_dummy, fusion->x1.x + kEps, fusion->x1.y + kEps,
                 fusion->x1.z + kEps);
        fusionPredict(fusion, &w_dummy, dT);
    }

    static float fake_mag_decimation = 0.f;
    if (!(fusion->flags & FUSION_USE_MAG) &&
        (fake_mag_decimation += dT) > FAKE_MAG_INTERVAL) {
        // game rotation mode, provide fake mag update to prevent
        // P to diverge over time
        struct Mat33 R;
        fusionGetRotationMatrix(fusion, &R);
        struct Vec3 m;
        mat33Apply(&m, &R, &fusion->Bm);

        fusionUpdate(fusion, &m, &fusion->Bm,
                      fusion->param.mag_stdev);
        fake_mag_decimation = 0.f;
    }

    struct Vec3 unityA = *a;
    vec3ScalarMul(&unityA, l_inv);

    // Adaptive acc weighting (trust acc less as it deviates from nominal g
    // more), acc_stdev *= e(sqrt(| |acc| - g_nominal|))
    //
    // The weighting equation comes from heuristics.
    float d = sqrtf(fabsf(l - NOMINAL_GRAVITY));

    float p = l_inv * fusion->param.acc_stdev * expf(d);

    fusionUpdate(fusion, &unityA, &fusion->Ba, p);

    return 0;
}

int fusionHandleMag(struct Fusion *fusion, const struct Vec3 *m) {
    if (!fusion_init_complete(fusion, MAG, m, 0.0f /* dT */)) {
        return -EINVAL;
    }

    float magFieldSq = vec3NormSquared(m);

    if (magFieldSq > MAX_VALID_MAGNETIC_FIELD_SQ) {
        // ALOGI("magField %.2f > %.2f", sqrtf(magFieldSq), MAX_VALID_MAGNETIC_FIELD);
        return -EINVAL;
    }
    if (magFieldSq < MIN_VALID_MAGNETIC_FIELD_SQ) {
        // ALOGI("magField %.2f < %.2f", sqrtf(magFieldSq), MIN_VALID_MAGNETIC_FIELD);
        return -EINVAL;
    }

    struct Mat33 R;
    fusionGetRotationMatrix(fusion, &R);

    struct Vec3 up;
    mat33Apply(&up, &R, &fusion->Ba);

    struct Vec3 east;
    vec3Cross(&east, m, &up);

    if (vec3NormSquared(&east) < MIN_VALID_CROSS_PRODUCT_MAG_SQ) {
        return -EINVAL;
    }

    struct Vec3 north;
    vec3Cross(&north, &up, &east);

    float invNorm = 1.0f / vec3Norm(&north);
    vec3ScalarMul(&north, invNorm);

    fusionUpdate(fusion, &north, &fusion->Bm,
                  fusion->param.mag_stdev * invNorm);

    return 0;
}

void fusionGetAttitude(const struct Fusion *fusion, struct Vec4 *attitude) {
    *attitude = fusion->x0;
}

void fusionGetBias(const struct Fusion *fusion, struct Vec3 *bias) {
    *bias = fusion->x1;
}

void fusionGetRotationMatrix(const struct Fusion *fusion, struct Mat33 *R) {
    quatToMatrix(R, &fusion->x0);
}