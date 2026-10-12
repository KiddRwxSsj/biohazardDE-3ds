import sys,glob
from PIL import Image, ImageDraw
d=sys.argv[1]; out=sys.argv[2]; cols=int(sys.argv[3]) if len(sys.argv)>3 else 2
fs=sorted(glob.glob(d+'/frame_*.png')); ims=[Image.open(f).convert('RGB') for f in fs]
W,H=ims[0].size; rows=(len(ims)+cols-1)//cols
sheet=Image.new('RGB',(W*cols,H*rows),(40,40,40))
for i,(f,im) in enumerate(zip(fs,ims)):
    ImageDraw.Draw(im).text((4,H-12),f.split('_')[-1][:-4],fill=(255,255,0))
    sheet.paste(im,((i%cols)*W,(i//cols)*H))
sheet.save(out); print(len(fs),sheet.size)
