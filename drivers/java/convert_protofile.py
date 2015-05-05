#!/usr/bin/env python

'''
Generates AST terms and serialization code for the Java driver
'''

import re
import os
import json
import codecs
import datetime
import os.path

from collections import OrderedDict
from mako.lookup import TemplateLookup

PACKAGE_DIR = './src/main/java/com/rethinkdb'
TEMPLATE_DIR = './templates'
PROTO_DIR = PACKAGE_DIR + '/proto'
AST_GEN_DIR = PACKAGE_DIR + '/ast/gen'
PROTO_FILE = '../../src/rdb_protocol/ql2.proto'
PROTO_JSON = './proto_basic.json'
META_JSON = './term_info.json'


def main():
    proto = get_proto_def()
    term_meta = get_term_metadata()
    new_json = diff_proto_keys(proto, term_meta)
    if new_json != term_meta:
        write_term_metadata(new_json)
        term_meta = new_json
    render_enums(proto)
    java_meta = java_specific_term_meta(term_meta)
    render_ast_subclasses(java_meta)
    generate_ast_files(java_meta)


def camel(varname):
    'CamelCase'
    return ''.join(x.title() for x in varname.split('_'))


def dromedary(words):
    'dromedaryCase'
    broken = words.split('_')
    return broken[0].lower() + ''.join(x.title() for x in broken[1:])


def autogenerated_header(template_path, output_path):
    rel_tpl = os.path.relpath(template_path, start=output_path)

    return ('// Autogenerated by {} on {}.\n'
            '// Do not edit this file directly.\n'
            '// The template for this file is located at:\n'
            '// {}\n').format(
        __file__[2:], datetime.date.today(), rel_tpl)


def get_proto_def():
    '''Get protocol definition json as a dictionary'''
    if not os.path.isfile(PROTO_JSON):
        print "proto_basic.json doesn't exist. creating from scratch"
        with open(PROTO_FILE) as ql2:
            proto = Proto2Dict(ql2)()
        with codecs.open(PROTO_JSON) as pb:
            json.dump(proto, pb, indent=2)
        return proto
    else:
        return json.load(
            codecs.open("proto_basic.json", "r", "utf-8"),
            object_pairs_hook=OrderedDict,
        )


def get_term_metadata():
    '''Gets some extra metadata needed to fully generate terms'''
    with codecs.open(META_JSON, "r", 'utf-8') as f:
        return json.load(f, object_pairs_hook=OrderedDict)


def write_term_metadata(term_meta):
    with codecs.open(META_JSON, 'w', 'utf-8') as f:
        json.dump(term_meta, f, indent=4)


def diff_proto_keys(proto, term_meta):
    '''Finds any new keys in the protobuf file and adds dummy entries
    for them in the term_info.json dictionary'''
    set_meta = set(term_meta.keys())
    proto_items = proto['Term']['TermType']
    diff = [x for x in proto_items.keys()
            if x not in set_meta]
    new = term_meta.copy()
    for key in diff:
        print "Got new term", key, "with id", proto_items[key],
        new[key] = OrderedDict({'id': proto_items[key]})
    # Sync up protocol ids (thess should never change, but it's best
    # that it's automated since they'd otherwise be specified in two
    # places that would need to be kept in sync.
    for key, val in new.iteritems():
        if val['id'] != proto_items[key]:
            print "Warning: {} changed from {} to {}".format(
                key, val['id'], proto_items[key])
            val['id'] = proto_items[key]
    return new


def java_specific_term_meta(term_meta):
    '''Returns a new term_meta that has java-specific changes. Should
    not be written out to a file, as it's language dependent.'''
    new_meta = term_meta.copy()
    java_keywords = {
        'abstract', 'continue', 'for', 'new', 'switch', 'assert',
        'default', 'goto', 'package', 'synchronized', 'boolean', 'do',
        'if', 'private', 'this', 'break', 'double', 'implements',
        'protected', 'throw', 'byte', 'else', 'import', 'public',
        'throws', 'case', 'enum', 'instanceof', 'return', 'transient',
        'catch', 'extends', 'int', 'short', 'try', 'char', 'final',
        'interface', 'static', 'void', 'class', 'finally', 'long',
        'strictfp', 'volatile', 'const', 'float', 'native', 'super',
        'while'
    }

    java_blacklist = {
        "row"  # Java 8 lambda syntax is nice, so skipping this
    }

    def term_called(info, name, set_):
        return dromedary(name) in set_ or info.get('alias') in set_

    for term_name, info in list(new_meta.items()):
        if term_called(info, term_name, java_blacklist):
            del new_meta[term_name]
        if term_called(info, term_name, java_keywords):
            info = info.copy()
            alias = info.get('alias', dromedary(term_name)) + '_'
            print "Alias for", term_name, "will be", alias
            info['alias'] = alias
            new_meta[term_name] = info
        if term_name == 'BRACKET':
            new_meta['BRACKET']['alias'] = 'field'
    return new_meta


def generate_ast_files(term_meta):
    '''Creates the AST files in com/rethinkdb/ast/query/gen'''
    print "Generating ast files"
    for key, value in term_meta.items():
        pass

# Used in parsing protofile
MESSAGE_REGEX = re.compile('\s*(message|enum) (?P<name>\w+) \{')
VALUE_REGEX = re.compile('\s*(?P<name>\w+)\s*=\s*(?P<value>\w+)')
END_REGEX = re.compile('\s*\}')


class Proto2Dict(object):
    def __init__(self, input_file):
        self._in = input_file
        self.d = OrderedDict()
        self.parents = []

    def __call__(self):
        for line in self._in:
            (self.match_message(line) or
             self.match_value(line) or
             self.match_end(line))
        while self.parents:
            self.pop_stack()
        return self.d

    def push_message(self, name):
        new_level = OrderedDict()
        self.d[name] = new_level
        self.parents.append(self.d)
        self.d = new_level

    def pop_stack(self):
        self.d = self.parents.pop()

    def match_message(self, line):
        match = MESSAGE_REGEX.match(line)
        if match is None:
            return False
        self.push_message(match.group('name'))
        return True

    def match_value(self, line):
        match = VALUE_REGEX.match(line)
        if match is None:
            return False
        self.d[match.group('name')] = int(match.group('value'), 0)
        return True

    def match_end(self, line):
        if END_REGEX.match(line):
            self.pop_stack()
            return True
        else:
            return False


TL = TemplateLookup(directories=[TEMPLATE_DIR])

template_context = {
    'camel': camel,  # CamelCase function
    'dromedary': dromedary,  # dromeDary case function
}


def render(template_name, output_dir, output_name=None, **kwargs):
    if output_name is None:
        output_name = template_name

    tpl = TL.get_template(template_name)
    output_path = output_dir + '/' + output_name
    with codecs.open(output_path, "w", "utf-8") as outfile:
        print "Rendering", output_path
        results = template_context.copy()
        results.update(kwargs)
        rendered = tpl.render(**results)
        outfile.write(autogenerated_header(
            TEMPLATE_DIR + '/' + template_name,
            output_path,
        ))
        outfile.write(rendered)


def render_enums(proto):
    '''Render protocol enums'''
    render_enum("Version", proto["VersionDummy"]["Version"])
    render_enum("Protocol", proto["VersionDummy"]["Protocol"])
    render_enum("QueryType", proto["Query"]["QueryType"])
    render_enum("FrameType", proto["Frame"]["FrameType"])
    render_enum("ResponseType", proto["Response"]["ResponseType"])
    render_enum("ResponseNote", proto["Response"]["ResponseNote"])
    render_enum("DatumType", proto["Datum"]["DatumType"])
    render_enum("TermType", proto["Term"]["TermType"])


def render_enum(classname, mapping):
    render("Enum.java",
           PROTO_DIR,
           output_name=classname+'.java',
           classname=classname,
           items=mapping.items(),
           )


def render_ast_subclass(
        term_type, include_in, meta, classname=None, superclass="RqlQuery"):
    '''Generates a RqlAst subclass. Either term_type or classname should
    be given'''
    classname = classname or camel(term_type)
    render("AstSubclass.java",
           AST_GEN_DIR,
           output_name=classname+'.java',
           term_type=term_type,
           classname=classname,
           meta=meta,
           include_in=include_in,
           superclass=superclass,
           )


def render_ast_subclasses(meta):
    special_superclasses = {
        "DB": "RqlAst",
        "RqlQuery": "RqlAst",
        "TopLevel": "RqlAst",
    }
    render_ast_subclass(
        term_type=None,
        include_in="query",
        meta=meta,
        classname="RqlQuery",
        superclass=special_superclasses["RqlQuery"],
    )
    render_ast_subclass(
        term_type=None,
        include_in="top",
        meta=meta,
        classname="TopLevel",
        superclass=special_superclasses["TopLevel"],
    )
    for term_name, info in list(meta.items()):
        if not info.get('deprecated'):
            render_ast_subclass(
                term_type=term_name,
                include_in=term_name.lower(),
                meta=meta,
                superclass=special_superclasses.get(term_name, "RqlQuery"),
                classname=None,
            )

if __name__ == '__main__':
    main()