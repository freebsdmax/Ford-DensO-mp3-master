t=open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
s='      <div class="card">\n        <div class="card-header"><h2>ELM327 Emulator</h2></div>'
print(s in t)
