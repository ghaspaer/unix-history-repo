#!/bin/sh
#
# mkextract - generate extract.sh
# Jordan Hubbard
#
# This script generates the extract.sh script from the current tarballs
# and should not be run by anyone but the release coordinator (there wouldn't
# be much point).
#
#					Jordan

BASEDIR=/usr/X11R6
TARGETS=XFree86-3.1*
echo -n "Creating extract.sh.."
cat > extract.sh << DO_THE_FUNKY_CHICKEN
#!/bin/sh
#
# Don't edit me - I'm auto-generated by mkextract.sh!
#
if [ ! -f /usr/bin/tar ]; then
	dialog --title "Error!" --msgbox "You must install the bindist before this distribution!" 6 72
	exit 0
fi

dialog --title "XFree86 3.1 Installation" \
  --msgbox "Welcome to the XFree86 3.1 installation!  You'll be asked
a series of annoying yes/no questions for each component of the
XFree86 distribution you wish to install.  If you're not sure
whether or not you need some component, simply answer yes and
delete it later if it turns out you don't need it.  This is
a little rough, yes, but I'm working on it!

Comments on the XFree86 distribution to David Dawes
<dawes@FreeBSD.org>

Comments on this install to Jordan Hubbard
<jkh@FreeBSD.org>

Thanks!" 18 72
dialog --title "Read This First" --textbox README.FreeBSD 22 76
DO_THE_FUNKY_CHICKEN

for i in $TARGETS; do
	abbrevname=`echo $i | sed -e 's/XFree86-3.1-//' -e 's/.tar.gz//'`
	echo "if dialog --title \"Install Request\" --yesno \"Do you wish to install the ${abbrevname} distribution?\" 6 72; then dialog --title \"Progress\" --infobox \"Installing $i\" 6 72; tar --unlink -xvzf $i -C /usr > /dev/ttyv1 2>&1 ; fi" >> extract.sh
done

cat >> extract.sh << OH_YEAH_BABY_GET_DOWN
dialog --title "Finished!" \
  --infobox "
You're now done with the installation of XFree86 3.1.
Now would probably be a very good time to look in ${BASEDIR}/lib/X11/doc
for further information on what to do next.  XFree86 3.1 is now
installed in the ${BASEDIR} directory, unlike
earlier releases.  For backwards compatibility, you might consider
a symlink to /usr/X386." 10 76
OH_YEAH_BABY_GET_DOWN

chmod 755 extract.sh
echo "  Done."
