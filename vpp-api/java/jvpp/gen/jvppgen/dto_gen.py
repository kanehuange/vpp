#!/usr/bin/env python
#
# Copyright (c) 2016 Cisco and/or its affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at:
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
from string import Template

import util

dto_template = Template("""
package $plugin_package.$dto_package;

/**
 * <p>This class represents $description.
 * <br>It was generated by dto_gen.py based on $inputfile preparsed data:
 * <pre>
$docs
 * </pre>
 */
public final class $cls_name implements $base_package.$dto_package.$base_type {

$fields
$methods
}
""")

field_template = Template("""    public $type $name;\n""")

send_template = Template("""    @Override
    public int send(final $base_package.JVpp jvpp) throws io.fd.vpp.jvpp.VppInvocationException {
        return (($plugin_package.JVpp${plugin_name})jvpp).$method_name($args);
    }""")


def generate_dtos(func_list, base_package, plugin_package, plugin_name, dto_package, inputfile):
    """ Generates dto objects in a dedicated package """
    print "Generating DTOs"

    if not os.path.exists(dto_package):
        raise Exception("%s folder is missing" % dto_package)

    for func in func_list:
        camel_case_dto_name = util.underscore_to_camelcase_upper(func['name'])
        camel_case_method_name = util.underscore_to_camelcase(func['name'])
        dto_path = os.path.join(dto_package, camel_case_dto_name + ".java")

        if util.is_ignored(func['name']) or util.is_control_ping(camel_case_dto_name):
            continue

        fields = generate_dto_fields(camel_case_dto_name, func)
        methods = generate_dto_base_methods(camel_case_dto_name, func)
        base_type = ""

        # Generate request/reply or dump/dumpReply even if structure can be used as notification
        if not util.is_just_notification(func["name"]):
            if util.is_reply(camel_case_dto_name):
                description = "reply DTO"
                request_dto_name = get_request_name(camel_case_dto_name, func['name'])
                if util.is_details(camel_case_dto_name):
                    # FIXME assumption that dump calls end with "Dump" suffix. Not enforced in vpe.api
                    base_type += "JVppReply<%s.%s.%s>" % (plugin_package, dto_package, request_dto_name + "Dump")
                    generate_dump_reply_dto(request_dto_name, base_package, plugin_package, dto_package,
                                            camel_case_dto_name, camel_case_method_name, func)
                else:
                    base_type += "JVppReply<%s.%s.%s>" % (plugin_package, dto_package, request_dto_name)
            else:
                args = "" if fields is "" else "this"
                methods += send_template.substitute(method_name=camel_case_method_name,
                                                    base_package=base_package,
                                                    plugin_package=plugin_package,
                                                    plugin_name=plugin_name,
                                                    args=args)
                if util.is_dump(camel_case_dto_name):
                    base_type += "JVppDump"
                    description = "dump request DTO"
                else:
                    base_type += "JVppRequest"
                    description = "request DTO"

            write_dto_file(base_package, plugin_package, base_type, camel_case_dto_name, description, dto_package,
                           dto_path, fields, func, inputfile, methods)

        # for structures that are also used as notifications, generate dedicated notification DTO
        if util.is_notification(func["name"]):
            base_type = "JVppNotification"
            description = "notification DTO"
            camel_case_dto_name = util.add_notification_suffix(camel_case_dto_name)
            dto_path = os.path.join(dto_package, camel_case_dto_name + ".java")
            methods = generate_dto_base_methods(camel_case_dto_name, func)
            write_dto_file(base_package, plugin_package, base_type, camel_case_dto_name, description, dto_package,
                           dto_path, fields, func, inputfile, methods)

    flush_dump_reply_dtos(inputfile)


def generate_dto_base_methods(camel_case_dto_name, func):
    methods = generate_dto_hash(func)
    methods += generate_dto_equals(camel_case_dto_name, func)
    methods += generate_dto_tostring(camel_case_dto_name, func)
    return methods


def generate_dto_fields(camel_case_dto_name, func):
    fields = ""
    for t in zip(func['types'], func['args']):
        # for retval don't generate dto field in Reply
        field_name = util.underscore_to_camelcase(t[1])
        if util.is_reply(camel_case_dto_name) and util.is_retval_field(field_name):
            continue
        fields += field_template.substitute(type=util.jni_2_java_type_mapping[t[0]],
                                            name=field_name)
    return fields


tostring_field_template = Template("""                \"$field_name=\" + $field_name + ", " +\n""")
tostring_array_field_template = Template("""                \"$field_name=\" + java.util.Arrays.toString($field_name) + ", " +\n""")
tostring_template = Template("""    @Override
    public String toString() {
        return "$cls_name{" +
$fields_tostring "}";
    }\n\n""")


def generate_dto_tostring(camel_case_dto_name, func):
    tostring_fields = ""
    for t in zip(func['types'], func['args']):

        field_name = util.underscore_to_camelcase(t[1])
        # for retval don't generate dto field in Reply
        if util.is_retval_field(field_name):
            continue

        # handle array types
        if util.is_array(util.jni_2_java_type_mapping[t[0]]):
            tostring_fields += tostring_array_field_template.substitute(field_name=field_name)
        else:
            tostring_fields += tostring_field_template.substitute(field_name=field_name)

    return tostring_template.substitute(cls_name=camel_case_dto_name,
                                        fields_tostring=tostring_fields[:-8])


equals_field_template = Template("""        if (!java.util.Objects.equals(this.$field_name, other.$field_name)) {
            return false;
        }\n""")
equals_array_field_template = Template("""        if (!java.util.Arrays.equals(this.$field_name, other.$field_name)) {
            return false;
        }\n""")
equals_template = Template("""    @Override
    public boolean equals(final Object o) {
        if (this == o) {
            return true;
        }
        if (o == null || getClass() != o.getClass()) {
            return false;
        }

        final $cls_name other = ($cls_name) o;

$comparisons
        return true;
    }\n\n""")


def generate_dto_equals(camel_case_dto_name, func):
    equals_fields = ""
    for t in zip(func['types'], func['args']):
        field_name = util.underscore_to_camelcase(t[1])
        # for retval don't generate dto field in Reply
        if util.is_retval_field(field_name):
            continue

        # handle array types
        if util.is_array(util.jni_2_java_type_mapping[t[0]]):
            equals_fields += equals_array_field_template.substitute(field_name=field_name)
        else:
            equals_fields += equals_field_template.substitute(field_name=field_name)

    return equals_template.substitute(cls_name=camel_case_dto_name,
                                      comparisons=equals_fields)


hash_template = Template("""    @Override
    public int hashCode() {
        return java.util.Objects.hash($fields);
    }\n\n""")
hash_single_array_type_template = Template("""    @Override
    public int hashCode() {
        return java.util.Arrays.hashCode($fields);
    }\n\n""")


def generate_dto_hash(func):
    hash_fields = ""

    # Special handling for hashCode in case just a single array field is present. Cannot use Objects.equals since the
    # array is mistaken for a varargs parameter. Instead use Arrays.hashCode in such case.
    if len(func['args']) == 1:
        single_type = func['types'][0]
        single_type_name = func['args'][0]
        if util.is_array(util.jni_2_java_type_mapping[single_type]):
            return hash_single_array_type_template.substitute(fields=util.underscore_to_camelcase(single_type_name))

    for t in zip(func['types'], func['args']):
        field_name = util.underscore_to_camelcase(t[1])
        # for retval don't generate dto field in Reply
        if util.is_retval_field(field_name):
            continue

        hash_fields += field_name + ", "

    return hash_template.substitute(fields=hash_fields[:-2])


def write_dto_file(base_package, plugin_package, base_type, camel_case_dto_name, description, dto_package, dto_path,
                   fields, func, inputfile, methods):
    dto_file = open(dto_path, 'w')
    dto_file.write(dto_template.substitute(inputfile=inputfile,
                                           description=description,
                                           docs=util.api_message_to_javadoc(func),
                                           cls_name=camel_case_dto_name,
                                           fields=fields,
                                           methods=methods,
                                           base_package=base_package,
                                           plugin_package=plugin_package,
                                           base_type=base_type,
                                           dto_package=dto_package))
    dto_file.flush()
    dto_file.close()


dump_dto_suffix = "ReplyDump"
dump_reply_artificial_dtos = {}


# Returns request name or special one from unconventional_naming_rep_req map
def get_request_name(camel_case_dto_name, func_name):
    return util.underscore_to_camelcase_upper(
        util.unconventional_naming_rep_req[func_name]) if func_name in util.unconventional_naming_rep_req \
        else util.remove_reply_suffix(camel_case_dto_name)


def flush_dump_reply_dtos(inputfile):
    for dump_reply_artificial_dto in dump_reply_artificial_dtos.values():
        dto_path = os.path.join(dump_reply_artificial_dto['dto_package'],
                                dump_reply_artificial_dto['cls_name'] + ".java")
        dto_file = open(dto_path, 'w')
        dto_file.write(dto_template.substitute(inputfile=inputfile,
                                               description="dump reply wrapper",
                                               docs=dump_reply_artificial_dto['docs'],
                                               cls_name=dump_reply_artificial_dto['cls_name'],
                                               fields=dump_reply_artificial_dto['fields'],
                                               methods=dump_reply_artificial_dto['methods'],
                                               plugin_package=dump_reply_artificial_dto['plugin_package'],
                                               base_package=dump_reply_artificial_dto['base_package'],
                                               base_type=dump_reply_artificial_dto['base_type'],
                                               dto_package=dump_reply_artificial_dto['dto_package']))
        dto_file.flush()
        dto_file.close()


def generate_dump_reply_dto(request_dto_name, base_package, plugin_package, dto_package, camel_case_dto_name,
                            camel_case_method_name, func):
    base_type = "JVppReplyDump<%s.%s.%s, %s.%s.%s>" % (
        plugin_package, dto_package, util.remove_reply_suffix(camel_case_dto_name) + "Dump",
        plugin_package, dto_package, camel_case_dto_name)
    fields = "    public java.util.List<%s> %s = new java.util.ArrayList<>();" % (camel_case_dto_name, camel_case_method_name)
    cls_name = camel_case_dto_name + dump_dto_suffix
    # using artificial type for fields, just to bypass the is_array check in base methods generators
    # the type is not really used
    artificial_type = 'jstring'

    # In case of already existing artificial reply dump DTO, just update it
    # Used for sub-dump dtos
    if request_dto_name in dump_reply_artificial_dtos.keys():
        dump_reply_artificial_dtos[request_dto_name]['fields'] += '\n' + fields
        dump_reply_artificial_dtos[request_dto_name]['field_names'].append(func['name'])
        dump_reply_artificial_dtos[request_dto_name]['field_types'].append(artificial_type)
        methods = '\n' + generate_dto_base_methods(dump_reply_artificial_dtos[request_dto_name]['cls_name'],
                                            {'args': dump_reply_artificial_dtos[request_dto_name]['field_names'],
                                             'types': dump_reply_artificial_dtos[request_dto_name]['field_types']})
        dump_reply_artificial_dtos[request_dto_name]['methods'] = methods
    else:
        methods = '\n' + generate_dto_base_methods(cls_name, {'args': [func['name']],
                                                              'types': [artificial_type]})
        dump_reply_artificial_dtos[request_dto_name] = ({'docs': util.api_message_to_javadoc(func),
                                                         'cls_name': cls_name,
                                                         'fields': fields,
                                                         'field_names': [func['name']],
                                                         'field_types': [artificial_type],
                                                         # strip too many newlines at the end of base method block
                                                         'methods': methods,
                                                         'plugin_package': plugin_package,
                                                         'base_package': base_package,
                                                         'base_type': base_type,
                                                         'dto_package': dto_package})
