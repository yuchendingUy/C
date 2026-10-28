/*
 * 机械动力服务端配置（照 infrastructure/config/CKinetics 的默认值）
 */
#ifndef MCC_CREATE_CONFIG_H
#define MCC_CREATE_CONFIG_H

namespace mcc::create {

struct KineticsConfig {
    bool disable_stress = false;
    int max_rotation_speed = 256;
    int kinetic_validation_frequency = 60;
    float medium_speed = 30;
    float fast_speed = 100;
};

extern KineticsConfig g_kinetics;

} // namespace mcc::create

#endif
