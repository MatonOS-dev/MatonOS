# Vendored D-Bus runtime

`dbus-java-core-5.2.2.jar` is the hypfvieh/dbus-java core artifact (MIT).
5.2.2 is the newest stable version selected for this port; move this pin to
6.x when upstream releases 6.x. `slf4j-api` and the no-op provider are pinned
at 2.0.17 and use the SLF4J MIT license. No transport or native artifact is
included. `SHA256SUMS` is checked by the app's Gradle preBuild task.

Sources:

- https://repo.maven.apache.org/maven2/com/github/hypfvieh/dbus-java-core/5.2.2/dbus-java-core-5.2.2.jar
- https://repo.maven.apache.org/maven2/org/slf4j/slf4j-api/2.0.17/slf4j-api-2.0.17.jar
- https://repo.maven.apache.org/maven2/org/slf4j/slf4j-nop/2.0.17/slf4j-nop-2.0.17.jar
