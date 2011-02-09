/*
 * svn_serializer.c: implement the tempoary structure serialization API
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <assert.h>
#include "private/svn_temp_serializer.h"
#include "svn_string.h"

/* This is a very efficient serialization and especially efficient
 * deserialization framework. The idea is just to concatenate all sub-
 * structures and strings into a single buffer while preserving proper
 * member alignment. Pointers will be replaced by the respective data
 * offsets in the buffer when that target that it pointed to gets
 * serialized, i.e. appended to the data buffer written so far.
 *
 * Hence, deserialization can be simply done by copying the buffer and
 * adjusting the pointers. No fine-grained allocation and copying is
 * necessary. 
 */

/* An element in the structure stack. It contains a pointer to the source
 * structure so that the relative offset of sub-structure or string
 * references can be determined properly. It also contains the corresponding
 * position within the serialized data. Thus, pointers can be serialized
 * as offsets within the target buffer.
 */
typedef struct source_stack_t
{
  /* the source structure passed in to *_init or *_push */
  const void *source_struct;

  /* offset within the target buffer to where the structure got copied */
  apr_size_t target_offset;

  /* parent stack entry. Will be NULL for the root entry. */
  struct source_stack_t *upper;
} source_stack_t;

/* Serialization context info. It basically consists of the buffer holding
 * the serialized result and the stack of source structure information.
 */
struct svn_temp_serializer__context_t
{
  /* allocations are made from this pool */
  apr_pool_t *pool;

  /* the buffer holding all serialized data */
  svn_stringbuf_t *buffer;

  /* the stack of structures being serialized. If NULL, the serialization
   * process has been finished. However, it is not necessarily NULL when
   * the application end serialization. */
  source_stack_t *source;
};

/* Mmake sure the serialized data len is a multiple of the default alignment,
 * i.e. structures may be appended without violating member alignment
 * guarantees.
 */
static void
align_buffer_end(svn_temp_serializer__context_t *context)
{
  apr_size_t current_len = context->buffer->len;
  apr_size_t aligned_len = APR_ALIGN_DEFAULT(current_len);
  if (aligned_len != current_len)
    {
      svn_stringbuf_ensure(context->buffer, aligned_len+1);
      context->buffer->len = aligned_len;
    }
}

/* Begin the serialization process for the SOURCE_STRUCT and all objects
 * referenced from it. STRUCT_SIZE must match the result of sizeof() of
 * the actual structure. You may suggest a larger initial buffer size
 * in SUGGESTED_BUFFER_SIZE to minimize the number of internal buffer
 * re-allocations during the serialization process. All allocations will
 * be made from POOL.
 */
svn_temp_serializer__context_t *
svn_temp_serializer__init(const void *source_struct,
                          apr_size_t struct_size,
                          apr_size_t suggested_buffer_size,
                          apr_pool_t *pool)
{
  /* select a meaningful initial memory buffer capacity */
  apr_size_t init_size = suggested_buffer_size < struct_size
                       ? struct_size
                       : suggested_buffer_size;

  /* create the serialization context and initialize it */
  svn_temp_serializer__context_t *context = apr_palloc(pool, sizeof(*context));
  context->pool = pool;
  context->buffer = svn_stringbuf_create_ensure(init_size, pool);

  /* If a source struct has been given, make it the root struct. */
  if (source_struct)
    {
      context->source = apr_palloc(pool, sizeof(*context->source));
      context->source->source_struct = source_struct;
      context->source->target_offset = 0;
      context->source->upper = NULL;

      /* serialize, i.e. append, the content of the first structure */
      svn_stringbuf_appendbytes(context->buffer, source_struct, struct_size);
    }
    else
    {
      /* The root struct will be set with the first push() op, or not at all
       * (in case of a plain string). */
      context->source = NULL;
    }

  /* done */
  return context;
}

/* Utility function replacing the serialized pointer corresponding to
 * *SOURCE_POINTER with the offset that it will be put when being append
 * right after this function call.
 */
static void
store_current_end_pointer(svn_temp_serializer__context_t *context,
                          const void * const * source_pointer)
{
  apr_size_t ptr_offset;
  apr_size_t *target_ptr;

  /* if *source_pointer is the root struct, there will be no parent structure
   * to relate it to */
  if (context->source == NULL)
    return;

  /* position of the serialized pointer relative to the begin of the buffer */
  ptr_offset = (const char *)source_pointer
             - (const char *)context->source->source_struct
             + context->source->target_offset;

  /* the offset must be within the serialized data. Otherwise, you forgot
   * to serialize the respective sub-struct. */
  assert(context->buffer->len > ptr_offset);

  /* use the serialized pointer as a storage for the offset */
  target_ptr = (apr_size_t*)(context->buffer->data + ptr_offset);

  /* store the current buffer length because that's where we will append
   * the serialized data of the sub-struct or string */
  *target_ptr = *source_pointer == NULL 
              ? 0
              : context->buffer->len - context->source->target_offset;
}

/* Begin serialization of a referenced sub-structure within the
 * serialization CONTEXT. SOURCE_STRUCT must be a reference to the pointer
 * in the original parent structure so that the correspondence in the
 * serialized structure can be established. STRUCT_SIZE must match the 
 * result of sizeof() of the actual structure.
 */
void
svn_temp_serializer__push(svn_temp_serializer__context_t *context,
                          const void * const * source_struct,
                          apr_size_t struct_size)
{
  /* create a new entry for the structure stack */
  source_stack_t *new = apr_palloc(context->pool, sizeof(*new));

  /* the serialized structure must be properly aligned */
  if (*source_struct)
    align_buffer_end(context);

  /* Store the offset at which the struct data that will the appended.
   * Write 0 for NULL pointers. */
  store_current_end_pointer(context, source_struct);

  /* store source and target information */
  new->source_struct = *source_struct;
  new->target_offset = context->buffer->len;

  /* put the new entry onto the stack*/
  new->upper = context->source;
  context->source = new;

  /* finally, actually append the new struct
   * (so we can now manipulate pointers within it) */
  if (*source_struct)
    svn_stringbuf_appendbytes(context->buffer, *source_struct, struct_size);
}

/* Remove the lastest structure from the stack.
 */
void
svn_temp_serializer__pop(svn_temp_serializer__context_t *context)
{
  /* we may pop the original struct but not further */
  assert(context->source);

  /* one level up the structure stack */
  context->source = context->source->upper;
}

/* Serialize a string referenced from the current structure within the
 * serialization CONTEXT. S must be a reference to the char* pointer in 
 * the original structure so that the correspondence in the serialized 
 * structure can be established.
 */
void
svn_temp_serializer__add_string(svn_temp_serializer__context_t *context,
                                const char * const * s)
{
  /* Store the offset at which the string data that will the appended.
   * Write 0 for NULL pointers. Strings don't need special alignment. */
  store_current_end_pointer(context, (const void **)s);

  /* append the string data */
  if (*s)
    svn_stringbuf_appendbytes(context->buffer, *s, strlen(*s) + 1);
}

/* Set the serialized representation of the pointer PTR inside the current
 * structure within the serialization CONTEXT to NULL. This is particularly
 * useful if the pointer is not NULL in the source structure.
 */
void
svn_temp_serializer__set_null(svn_temp_serializer__context_t *context,
                              const void * const * ptr)
{
  apr_size_t offset;

  /* there must be a parent structure */
  assert(context->source);

  /* position of the serialized pointer relative to the begin of the buffer */
  offset = (const char *)ptr
         - (const char *)context->source->source_struct
         + context->source->target_offset;

  /* the offset must be within the serialized data. Otherwise, you forgot
   * to serialize the respective sub-struct. */
  assert(context->buffer->len > offset);

  /* use the serialized pointer as a storage for the offset */
  *(apr_size_t*)(context->buffer->data + offset) = 0;
}

/* Return the the data buffer that receives the serialialized data from
 * the given serialization CONTEXT.
 */
svn_stringbuf_t *
svn_temp_serializer__get(svn_temp_serializer__context_t *context)
{
  return context->buffer;
}

/* Replace the deserialized pointer value at PTR inside BUFFER with a 
 * proper pointer value.
 */
void
svn_temp_deserializer__resolve(void *buffer, void **ptr)
{
  if ((apr_size_t)*ptr)
    {
      /* replace the offset in *ptr with the pointer to buffer[*ptr] */
      (*(const char **)ptr) = (const char*)buffer + (apr_size_t)*ptr;
      assert(*ptr > buffer);
    }
  else
    {
      /* NULL pointers are stored as 0 which might have a different
       * binary representation. */
      *ptr = NULL;
    }
}

const void *
svn_temp_deserializer__ptr(const void *buffer, const void **ptr)
{
  return (apr_size_t)*ptr == 0
      ? NULL
      : (const char*)buffer + (apr_size_t)*ptr;
}
