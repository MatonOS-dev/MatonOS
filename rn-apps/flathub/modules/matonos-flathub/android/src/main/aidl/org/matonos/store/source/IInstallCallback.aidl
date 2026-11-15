package org.matonos.store.source;

/** The store commits the APKs itself; sources never install. */
oneway interface IInstallCallback {
    void progress(String operationId, String phase, int percent, long bytesDone, long bytesTotal);
    /** All APKs of one package (base first, then splits), ready to commit. */
    void apks(String operationId, String packageName, in ParcelFileDescriptor[] files);
    void failed(String operationId, String error);
}
