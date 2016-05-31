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
import util
from string import Template

from util import remove_suffix

callback_suffix = "Callback"

callback_template = Template("""
package $base_package.$callback_package;

/**
 * <p>Represents callback for vpe.api message.
 * <br>It was generated by callback_gen.py based on $inputfile preparsed data:
 * <pre>
$docs
 * </pre>
 */
public interface $cls_name extends $base_package.$callback_package.$callback_type {

    $callback_method

}
""")

global_callback_template = Template("""
package $base_package.$callback_package;

/**
 * <p>Global aggregated callback interface.
 * <br>It was generated by callback_gen.py based on $inputfile
 * <br>(python representation of vpe.api generated by vppapigen).
 */
public interface JVppGlobalCallback extends $callbacks {
}
""")


def generate_callbacks(func_list, base_package, callback_package, dto_package, inputfile):
    """ Generates callback interfaces """
    print "Generating Callback interfaces"

    if not os.path.exists(callback_package):
        raise Exception("%s folder is missing" % callback_package)

    callbacks = []
    for func in func_list:

        if util.is_ignored(func['name']):
            continue

        camel_case_name_with_suffix = util.underscore_to_camelcase_upper(func['name'])
        if not util.is_reply(camel_case_name_with_suffix) and not util.is_notification(func['name']):
            continue

        if util.is_reply(camel_case_name_with_suffix):
            camel_case_name = util.remove_reply_suffix(camel_case_name_with_suffix)
            callback_type = "JVppCallback"
        else:
            camel_case_name_with_suffix = util.add_notification_suffix(camel_case_name_with_suffix)
            camel_case_name = camel_case_name_with_suffix
            callback_type = "JVppNotificationCallback"

        callbacks.append("{0}.{1}.{2}".format(base_package, callback_package, camel_case_name + callback_suffix))
        callback_path = os.path.join(callback_package, camel_case_name + callback_suffix + ".java")
        callback_file = open(callback_path, 'w')

        reply_type = "%s.%s.%s" % (base_package, dto_package, camel_case_name_with_suffix)
        method = "void on{0}({1} reply);".format(camel_case_name_with_suffix, reply_type)
        callback_file.write(
            callback_template.substitute(inputfile=inputfile,
                                         docs=util.api_message_to_javadoc(func),
                                         cls_name=camel_case_name + callback_suffix,
                                         callback_method=method,
                                         base_package=base_package,
                                         callback_package=callback_package,
                                         callback_type=callback_type))
        callback_file.flush()
        callback_file.close()

    callback_file = open(os.path.join(callback_package, "JVppGlobalCallback.java"), 'w')
    callback_file.write(global_callback_template.substitute(inputfile=inputfile,
                                                            callbacks=", ".join(callbacks),
                                                            base_package=base_package,
                                                            callback_package=callback_package))
    callback_file.flush()
    callback_file.close()
