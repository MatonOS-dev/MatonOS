# Shared changes requested

- `gms/README.md`: remove its historical Neo Store paragraphs and test steps
  (including the obsolete claim that Neo Store replaces F-Droid Basic). The
  app-selection decision has changed: Neo Store is removed and the source-
  built privileged F-Droid Basic app now owns silent installs. Left untouched
  because `gms/` belongs to the GMS area.
- `setup/matonos-setup.sh`: no edit is needed. There is no existing
  install-unknown-apps grant in the script, and the privileged
  `INSTALL_PACKAGES` path lets F-Droid Basic use PackageInstaller sessions
  without such a per-app user setting.
