const path = require('path');

module.exports = {
  project: {
    android: {sourceDir: '.', packageName: 'org.matonos.shell'},
  },
  dependencies: {
    '@matonos/rn-common': {
      platforms: {
        android: {
          sourceDir: path.resolve(__dirname, '../rn-common/android'),
          packageImportPath: 'import org.matonos.rncommon.MatonOSPackage;',
          packageInstance: 'new MatonOSPackage()',
        },
      },
    },
  },
};
