#ifndef FLOWER_CONFIG_H
#define FLOWER_CONFIG_H

#define FLOWER_AUDIO_HOP                256u

#define FLOWER_AUDIO_FS_HZ              47991.0f

#define FLOWER_ENV_ATTACK_S             0.020f
#define FLOWER_ENV_RELEASE_S            0.250f

#define FLOWER_FLOOR_FALL_S             1.0f
#define FLOWER_FLOOR_RISE_S             30.0f
#define FLOWER_FLOOR_INIT_DBFS          (-70.0f)
#define FLOWER_FLOOR_MIN_DBFS           (-90.0f)
#define FLOWER_FLOOR_MAX_DBFS           (-18.0f)

#define FLOWER_GATE_MARGIN_DB           2.0f
#define FLOWER_GATE_SPAN_DB             14.0f
#define FLOWER_GATE_KNEE                0.25f

#define FLOWER_AUDIO_BANDS              1
#define FLOWER_BAND_LOW_HZ              250.0f
#define FLOWER_BAND_HIGH_HZ             2000.0f

#define FLOWER_SPRING_OPEN_FREQ_HZ      3.0f
#define FLOWER_SPRING_OPEN_DAMPING      1.0f

#define FLOWER_SPRING_SCALE_FREQ_HZ     2.4f
#define FLOWER_SPRING_SCALE_DAMPING     1.0f

#define FLOWER_SPRING_GLOW_FREQ_HZ      4.5f
#define FLOWER_SPRING_GLOW_DAMPING      1.0f

#define FLOWER_DT_MAX_S                 0.050f
#define FLOWER_DT_SUBSTEP_S             0.008f

#define FLOWER_BREATHE_HZ               0.17f
#define FLOWER_BREATHE_SCALE_AMP        0.013f
#define FLOWER_BREATHE_OPEN_AMP         0.040f
#define FLOWER_BREATHE_GLOW_AMP         0.045f

#define FLOWER_PETAL_PHASE_GOLDEN       0.6180339887f
#define FLOWER_PETAL_PHASE_SPREAD       0.85f

#define FLOWER_FLORET_PHASE_GOLDEN      0.3819660113f
#define FLOWER_FLORET_BREATHE_SCALE     1.7f

#define FLOWER_BLOOM_S                  1.80f
#define FLOWER_BLOOM_SCALE_FROM         0.88f
#define FLOWER_BLOOM_SCALE_TO           1.00f
#define FLOWER_BLOOM_STAGGER_S          0.60f

#define FLOWER_WILT_ROT_DEG             30.0f
#define FLOWER_WILT_ROT_S               1.00f
#define FLOWER_WILT_TOTAL_S             4.00f
#define FLOWER_WILT_SCALE_TO            0.94f

#define FLOWER_DORMANT_POLL_MS          20u

#define FLOWER_PRESENCE_EPSILON         0.004f

#define FLOWER_REST_OPEN                0.58f
#define FLOWER_ENERGY_TO_OPEN           0.55f
#define FLOWER_ENERGY_TO_SCALE          0.22f
#define FLOWER_ENERGY_TO_GLOW           0.85f
#define FLOWER_GLOW_IDLE_ALPHA          0.15f

#define FLOWER_PETAL_PUSH_PX            24.0f
#define FLOWER_FLORET_PUSH_PX           18.0f

#define FLOWER_TWIG_SCALE_FOLLOW        0.28f
#define FLOWER_ENERGY_TO_TWIG_PX        14.0f

#define FLOWER_BAND_HIGH_TO_FLORET      0.55f
#define FLOWER_BAND_LOW_TO_SCALE        0.45f

#define FLOWER_JTAG_TAKE                0
#define FLOWER_JTAG_KEEP                1
#define FLOWER_JTAG_AUTO                2

#ifndef FLOWER_KEEP_JTAG_PINS
#define FLOWER_KEEP_JTAG_PINS           FLOWER_JTAG_AUTO
#endif

#ifndef FLOWER_GFX_WRITEBACK
#define FLOWER_GFX_WRITEBACK            1
#endif

#ifndef FLOWER_WARP_MVE
#define FLOWER_WARP_MVE                 1
#endif

#ifndef FLOWER_WARP_TILE
#define FLOWER_WARP_TILE                128
#endif

#define FLOWER_WARP_IDENTITY_SCALE_EPS  0.0015f
#define FLOWER_WARP_IDENTITY_ROT_EPS    0.05f

#define FLOWER_POSE_SNAP_LO             0.02f
#define FLOWER_POSE_SNAP_HI             0.98f

#define FLOWER_ENABLE_DCACHE            1

#define FLOWER_CONFIG_RIF               1

#ifndef FLOWER_SET_CPU_CLOCK
#define FLOWER_SET_CPU_CLOCK            1
#endif

#ifndef FLOWER_MEMBENCH
#define FLOWER_MEMBENCH                 0
#endif

#ifndef FLOWER_WARP_PROFILE
#define FLOWER_WARP_PROFILE             0
#endif

#define FLOWER_TELEMETRY_MS             500u

#endif /* FLOWER_CONFIG_H */
