t = open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
print(t.count('component_logs_get_uri'))
print(t.count('component_logs_post_uri'))
