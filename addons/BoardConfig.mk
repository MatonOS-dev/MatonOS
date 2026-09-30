# Add-ons use the fixed, shared MatonOS SELinux policy set in
# sepolicy/matonos/. Do not add an area policy directory: it would re-trigger
# a full Soong policy graph and violates the single matonos_driver policy set.
