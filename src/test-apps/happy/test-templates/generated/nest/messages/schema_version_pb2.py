#
#   Copyright (c) 2019 Google LLC.
#   Copyright (c) 2016-2018 Nest Labs, Inc.
#   All rights reserved.
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.
#
# Generated by the protocol buffer compiler.  DO NOT EDIT!
# source: nest/messages/schema_version.proto

from __future__ import absolute_import
import sys
_b=sys.version_info[0]<3 and (lambda x:x) or (lambda x:x.encode('latin1'))
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from google.protobuf import reflection as _reflection
from google.protobuf import symbol_database as _symbol_database
from google.protobuf import descriptor_pb2
# @@protoc_insertion_point(imports)

_sym_db = _symbol_database.Default()




DESCRIPTOR = _descriptor.FileDescriptor(
  name='nest/messages/schema_version.proto',
  package='nest.messages',
  syntax='proto3',
  serialized_pb=_b('\n\"nest/messages/schema_version.proto\x12\rnest.messages\"D\n\rSchemaVersion\x12\x17\n\x0f\x63urrent_version\x18\x01 \x01(\r\x12\x1a\n\x12min_compat_version\x18\x02 \x01(\rB\x06\xa2\x02\x03PCLb\x06proto3')
)




_SCHEMAVERSION = _descriptor.Descriptor(
  name='SchemaVersion',
  full_name='nest.messages.SchemaVersion',
  filename=None,
  file=DESCRIPTOR,
  containing_type=None,
  fields=[
    _descriptor.FieldDescriptor(
      name='current_version', full_name='nest.messages.SchemaVersion.current_version', index=0,
      number=1, type=13, cpp_type=3, label=1,
      has_default_value=False, default_value=0,
      message_type=None, enum_type=None, containing_type=None,
      is_extension=False, extension_scope=None,
      options=None, file=DESCRIPTOR),
    _descriptor.FieldDescriptor(
      name='min_compat_version', full_name='nest.messages.SchemaVersion.min_compat_version', index=1,
      number=2, type=13, cpp_type=3, label=1,
      has_default_value=False, default_value=0,
      message_type=None, enum_type=None, containing_type=None,
      is_extension=False, extension_scope=None,
      options=None, file=DESCRIPTOR),
  ],
  extensions=[
  ],
  nested_types=[],
  enum_types=[
  ],
  options=None,
  is_extendable=False,
  syntax='proto3',
  extension_ranges=[],
  oneofs=[
  ],
  serialized_start=53,
  serialized_end=121,
)

DESCRIPTOR.message_types_by_name['SchemaVersion'] = _SCHEMAVERSION
_sym_db.RegisterFileDescriptor(DESCRIPTOR)

SchemaVersion = _reflection.GeneratedProtocolMessageType('SchemaVersion', (_message.Message,), dict(
  DESCRIPTOR = _SCHEMAVERSION,
  __module__ = 'nest.messages.schema_version_pb2'
  # @@protoc_insertion_point(class_scope:nest.messages.SchemaVersion)
  ))
_sym_db.RegisterMessage(SchemaVersion)


DESCRIPTOR.has_options = True
DESCRIPTOR._options = _descriptor._ParseOptions(descriptor_pb2.FileOptions(), _b('\242\002\003PCL'))
# @@protoc_insertion_point(module_scope)
