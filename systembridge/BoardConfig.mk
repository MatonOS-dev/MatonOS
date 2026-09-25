# The bridge keeps its dedicated system_ext app domain. Its stable vendor Binder channel ACL
# is part of sepolicy/matonos. Root BoardConfig.mk must include this file.
SYSTEM_EXT_PUBLIC_SEPOLICY_DIRS += device/maton/pc_x86_64/systembridge/sepolicy/system_ext/public
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/maton/pc_x86_64/systembridge/sepolicy/system_ext/private
