t=open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
i=t.find('static const char *INDEX_HTML')
if i>=0:
    j=t.find('static esp_err_t root_get_handler', i)
    if j>=0:
        print(repr(t[j-300:j]))
    else:
        print('noj')
else:
    print('noi')
