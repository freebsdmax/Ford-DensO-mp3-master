t=open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
i=t.find('static const char *INDEX_HTML')
if i>=0:
    # find first quote
    q=t.find('"', i)
    if q>=0:
        print(repr(t[q:q+100]))
    else:
        print('noq')
else:
    print('noi')
