# lib_common_enc — encoder-side common data structures and settings

add_library(lib_common_enc_obj OBJECT
    EncBuffers.c
    IpEncFourCC.c
    EncSize.c
    EncHwScalingList.c
    Settings.c
    DPBConstraints.c
    ParamConstraints.c
    RateCtrlMeta.c
    QPTable.c
)
