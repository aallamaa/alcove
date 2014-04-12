print """#ifndef CHAR_H
#define CHAR_H
"""

v=['NUL', 'SOH', 'STX', 'ETX', 'EOT', 'ENQ', 'ACK', 'BEL', 'BS', 'HT', 'LF', 'VT', 'FF', 'CR', 'SO', 'SI', 'DLE', 'DC1', 'DC2', 'DC3', 'DC4', 'NAK', 'SYN', 'ETB', 'CAN', 'EM', 'SUB', 'ESC', 'FS', 'GS', 'RS', 'US', 'SPACE']
w=['Null character', 'Start of Header', 'Start of Text', 'End of Text', 'End of Transmission', 'Enquiry', 'Acknowledgment', 'Bell', 'Backspace[d][e]', 'Horizontal Tab[f]', 'Line feed', 'Vertical Tab', 'Form feed', 'Carriage return[g]', 'Shift Out', 'Shift In', 'Data Link Escape', 'Device Control 1 (oft. XON)', 'Device Control 2', 'Device Control 3 (oft. XOFF)', 'Device Control 4', 'Negative Acknowledgement', 'Synchronous idle', 'End of Transmission Block', 'Cancel', 'End of Medium', 'Substitute', 'Escape[i]', 'File Separator', 'Group Separator', 'Record Separator', 'Unit Separator','Space']

whitespace=[9,10,11,12,13,32]
singleescape=[ord("\\")]
multipleescape=[ord('"')]
termmacro=[ord(i) for i in "'(),;`|{}[]"]
ntermmacro=[ord(i) for i in "#"]
digit=[ord(i) for i in "0123456789"]
constituent=[ord(i) for i in ":!$%&*+-./<=>?@^_0123456789~"]+range(ord("a"),ord("z")+1)+range(ord("A"),ord("Z")+1)
hexa=[ord(i) for i in "0123456789abcdefABCDEF"]
iswhitespace=1
print "#define ISWHITESPACE %d" % (iswhitespace)
issingleescape=2
print "#define ISSINGLEESCAPE %d" % (issingleescape)
ismultipleescape=4
print "#define ISMULTIPLEESCAPE %d" % (ismultipleescape)
istermmacro=8
print "#define ISTERMMACRO %d" % (istermmacro)
isntermmacro=16
print "#define ISNTERMMACRO %d" % (isntermmacro)
isconstituent=32
print "#define ISCONSTITUENT %d" % (isconstituent)
isdigit=64
print "#define ISDIGIT %d" % (isdigit)
ishexa=128
print "#define ISHEXA %d" % (isdigit)



print "unsigned char chrmap[]={ "
chrl=[]
for i in range(256):
	flags=iswhitespace*(i in whitespace)+issingleescape*(i in singleescape)+ismultipleescape*(i in multipleescape)+istermmacro*(i in termmacro)+isntermmacro*(i in ntermmacro)+isconstituent*(i in constituent)+isdigit*(i in digit)+ishexa*(i in hexa)
	if (i<33):
		chrl.append( "%3d /* %s %s */" % (flags,v[i],w[i]))
	else:
		if (i<128):
			chrl.append( "%3d /* %s */" % (flags,chr(i)))
		else:
			chrl.append( "%3d /* %d */" % (flags,i))

for i in range(33):
	print "%s," % (chrl[i])

j=33;
for i in range(33,256):
	if sum(len(a) for a in chrl[j:i])>60:
		print ",\t".join(chrl[j:i])+","
		j=i

print ",\t".join(chrl[j:256])

		
print "};"

schrmap=dict(b="\b",f="\f",n="\n",r="\r",t="\t",v="\v")
schrmapi=[0]*256
for v in schrmap:
  schrmapi[ord(v)]=ord(schrmap[v])
print "unsigned char schrmap[]={"
print ",\t".join(str(i) for i in schrmapi)
print "};"

chr2hex=[-1]*256

for tm in [zip(range(ord('a'),ord('f')),range(10,16)),
           zip(range(ord('A'),ord('F')),range(10,16)),
           zip(range(ord('0'),ord('9')),range(0,10))]:
  for a,i in tm:
    chr2hex[a]=i

print "short int chr2hex[]={"
print ",\t".join(str(i) for i in chr2hex)
print "};"
           

print "#endif /* CHAR_H */"
