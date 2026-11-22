#!/system/bin/sh
ui_print "- Henge Gunyah host modules (DroidVM's, GPL-2.0)"
case "$(uname -r)" in
  6.1.*-android14-*) ui_print "- kernel $(uname -r): android14-6.1, as the modules are built for" ;;
  *) ui_print "! kernel $(uname -r) is not android14-6.1: the modules will not load on it" ;;
esac
set_perm "$MODPATH/service.sh" 0 0 0755
