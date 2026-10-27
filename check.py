t = open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
print(t.find('elm327_config_get_handler'), t.find('/elm327_config'))
