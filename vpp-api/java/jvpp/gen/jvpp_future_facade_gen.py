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

import os, util
from string import Template

import dto_gen

jvpp_facade_callback_template = Template("""
package $base_package.$future_package;

/**
 * Async facade callback setting values to future objects
 */
public final class FutureJVppFacadeCallback implements $base_package.$callback_package.JVppGlobalCallback {

    private final java.util.Map<java.lang.Integer, java.util.concurrent.CompletableFuture<? extends $base_package.$dto_package.JVppReply<?>>> requests;

    public FutureJVppFacadeCallback(final java.util.Map<java.lang.Integer, java.util.concurrent.CompletableFuture<? extends $base_package.$dto_package.JVppReply<?>>> requestMap) {
        this.requests = requestMap;
    }

$methods
}
""")

jvpp_facade_callback_method_template = Template("""
    @Override
    @SuppressWarnings("unchecked")
    public void on$callback_dto($base_package.$dto_package.$callback_dto reply) {
        final java.util.concurrent.CompletableFuture<$base_package.$dto_package.JVppReply<?>> completableFuture;

        synchronized(requests) {
            completableFuture = (java.util.concurrent.CompletableFuture<$base_package.$dto_package.JVppReply<?>>) requests.get(reply.context);
        }

        if(completableFuture != null) {
            if(reply.retval < 0) {
                completableFuture.completeExceptionally(new Exception("Invocation of " + $base_package.$dto_package.$callback_dto.class
                    + " failed with value " + reply.retval));
            } else {
                completableFuture.complete(reply);
            }

            synchronized(requests) {
                requests.remove(reply.context);
            }
        }
    }
""")

# TODO reuse common parts with generic method callback
jvpp_facade_control_ping_method_template = Template("""
    @Override
    @SuppressWarnings("unchecked")
    public void on$callback_dto($base_package.$dto_package.$callback_dto reply) {
        final java.util.concurrent.CompletableFuture<$base_package.$dto_package.JVppReply<?>> completableFuture;

        synchronized(requests) {
            completableFuture = (java.util.concurrent.CompletableFuture<$base_package.$dto_package.JVppReply<?>>) requests.get(reply.context);
        }

        if(completableFuture != null) {
            // Finish dump call
            if (completableFuture instanceof $base_package.$future_package.FutureJVppFacade.CompletableDumpFuture) {
                completableFuture.complete((($base_package.$future_package.FutureJVppFacade.CompletableDumpFuture) completableFuture).getReplyDump());
                // Remove future mapped to dump call context id
                synchronized(requests) {
                    requests.remove((($base_package.$future_package.FutureJVppFacade.CompletableDumpFuture) completableFuture).getContextId());
                }
            } else {
                if(reply.retval < 0) {
                    completableFuture.completeExceptionally(new Exception("Invocation of " + $base_package.$dto_package.$callback_dto.class
                        + " failed with value " + reply.retval));
                } else {
                    completableFuture.complete(reply);
                }
            }

            synchronized(requests) {
                requests.remove(reply.context);
            }
        }
    }
""")

jvpp_facade_details_callback_method_template = Template("""
    @Override
    @SuppressWarnings("unchecked")
    public void on$callback_dto($base_package.$dto_package.$callback_dto reply) {
        final FutureJVppFacade.CompletableDumpFuture<$base_package.$dto_package.$callback_dto_reply_dump> completableFuture;

        synchronized(requests) {
            completableFuture = ($base_package.$future_package.FutureJVppFacade.CompletableDumpFuture<$base_package.$dto_package.$callback_dto_reply_dump>) requests.get(reply.context);
        }

        if(completableFuture != null) {
            $base_package.$dto_package.$callback_dto_reply_dump replyDump = completableFuture.getReplyDump();
            if(replyDump == null) {
                replyDump = new $base_package.$dto_package.$callback_dto_reply_dump();
                completableFuture.setReplyDump(replyDump);
            }

            replyDump.$callback_dto_field.add(reply);
        }
    }
""")


def generate_jvpp(func_list, base_package, dto_package, callback_package, future_facade_package):
    """ Generates JVpp interface and JNI implementation """
    print "Generating JVpp future facade"

    if not os.path.exists(future_facade_package):
        raise Exception("%s folder is missing" % future_facade_package)

    callbacks = []
    for func in func_list:

        if util.is_notification(func['name']) or util.is_ignored(func['name']):
            # TODO handle notifications
            continue

        camel_case_name_with_suffix = util.underscore_to_camelcase_upper(func['name'])
        if not util.is_reply(camel_case_name_with_suffix):
            continue

        if util.is_details(camel_case_name_with_suffix):
            camel_case_method_name = util.underscore_to_camelcase(func['name'])
            camel_case_request_name = get_standard_dump_reply_name(util.underscore_to_camelcase_upper(func['name']),
                                                                   func['name'])
            callbacks.append(jvpp_facade_details_callback_method_template.substitute(base_package=base_package,
                                                                                     dto_package=dto_package,
                                                                                     callback_dto=camel_case_name_with_suffix,
                                                                                     callback_dto_field=camel_case_method_name,
                                                                                     callback_dto_reply_dump=camel_case_request_name + dto_gen.dump_dto_suffix,
                                                                                     future_package=future_facade_package))
        else:
            if util.is_control_ping(camel_case_name_with_suffix):
                callbacks.append(jvpp_facade_control_ping_method_template.substitute(base_package=base_package,
                                                                                     dto_package=dto_package,
                                                                                     callback_dto=camel_case_name_with_suffix,
                                                                                     future_package=future_facade_package))
            else:
                callbacks.append(jvpp_facade_callback_method_template.substitute(base_package=base_package,
                                                                                 dto_package=dto_package,
                                                                                 callback_dto=camel_case_name_with_suffix))

    jvpp_file = open(os.path.join(future_facade_package, "FutureJVppFacadeCallback.java"), 'w')
    jvpp_file.write(jvpp_facade_callback_template.substitute(base_package=base_package,
                                                             dto_package=dto_package,
                                                             callback_package=callback_package,
                                                             methods="".join(callbacks),
                                                             future_package=future_facade_package))
    jvpp_file.flush()
    jvpp_file.close()


# Returns request name or special one from unconventional_naming_rep_req map
def get_standard_dump_reply_name(camel_case_dto_name, func_name):
    # FIXME this is a hotfix for sub-details callbacks
    # FIXME also for L2FibTableEntry
    # It's all because unclear mapping between
    #  request -> reply,
    #  dump -> reply, details,
    #  notification_start -> reply, notifications

    # vpe.api needs to be "standardized" so we can parse the information and create maps before generating java code
    suffix = func_name.split("_")[-1]
    return util.underscore_to_camelcase_upper(
        util.unconventional_naming_rep_req[func_name]) + util.underscore_to_camelcase_upper(suffix) if func_name in util.unconventional_naming_rep_req \
        else camel_case_dto_name
