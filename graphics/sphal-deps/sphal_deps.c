/* Anchor for the copied (prebuilt) Mesa: it is installed with
 * PRODUCT_COPY_FILES, so Soong never sees its dependencies. Mesa dlopens
 * libhardware inside the sphal namespace for both ABIs. */
