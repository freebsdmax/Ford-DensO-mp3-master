t = open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
idx = t.rfind('httpd_register_uri_handler')
if idx >= 0:
    print(t[idx-150:idx+200])
else:
    print('no')
