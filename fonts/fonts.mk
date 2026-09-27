# MatonOS system icon font (user decision 2026-09-27): Material Symbols as a
# named system family apps can use by name; never a fallback or selectable font.
PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/fonts/MaterialSymbolsOutlined.ttf:$(TARGET_COPY_OUT_PRODUCT)/fonts/MaterialSymbolsOutlined.ttf \
    device/maton/pc_x86_64/fonts/fonts_customization.xml:$(TARGET_COPY_OUT_PRODUCT)/etc/fonts_customization.xml \
    device/maton/pc_x86_64/fonts/LICENSE:$(TARGET_COPY_OUT_PRODUCT)/fonts/MaterialSymbols-LICENSE
