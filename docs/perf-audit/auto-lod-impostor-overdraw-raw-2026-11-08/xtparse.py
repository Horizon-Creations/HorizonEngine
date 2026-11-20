import sys, xml.etree.ElementTree as ET
def load(path):
    root=ET.parse(path).getroot()
    ids={}
    for e in root.iter():
        if 'id' in e.attrib: ids[e.attrib['id']]=e
    def res(e):
        return ids[e.attrib['ref']] if 'ref' in e.attrib else e
    schema=[c.find('mnemonic').text for c in root.find('.//schema').findall('col')]
    rows=[]
    for r in root.iter('row'):
        d={}
        for name,c in zip(schema,list(r)):
            c=res(c); d[name]=(c.attrib.get('fmt'), c.text, c)
        rows.append(d)
    return schema,rows
