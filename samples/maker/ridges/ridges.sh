# Mountain ridges for the Generator piece: see ridges.awk. The C locale
# keeps awk's strings bytes, so the half blocks go out as CP437.
LC_ALL=C
export LC_ALL
exec awk -f ./ridges.awk
