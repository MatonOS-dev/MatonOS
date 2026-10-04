/* Optional local hook: run only on a disposable target with /dev/uinput.
 * No socket protocol, Android inventory, images or adb required. */
#include "../SessionPads.h"
#include <stdio.h>
#include <unistd.h>
static void rumble(void *context,unsigned slot,uint16_t strong,uint16_t weak) {
    (void)context;fprintf(stderr,"rumble slot=%u strong=%u weak=%u\n",slot,strong,weak);
}
int main(void) {
    MatonPadInventory pad={.descriptor="local-test-pad",.rumble=1};
    MatonSessionPads *session=maton_pads_create(&pad,1,10000,rumble,NULL);
    if(!session){perror("create session pad");return 1;}
    printf("%s\nPress Enter to destroy the pad.\n",maton_pads_nodes(session));
    int input=getchar();(void)input;maton_pads_destroy(session);return 0;
}
