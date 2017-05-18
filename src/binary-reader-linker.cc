/*
 * Copyright 2017 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "binary-reader-linker.h"

#include <vector>

#include "binary-reader-nop.h"
#include "wasm-link.h"

#define RELOC_SIZE 5

namespace wabt {
namespace link {

namespace {

class BinaryReaderLinker : public BinaryReaderNop {
 public:
  explicit BinaryReaderLinker(LinkerInputBinary* binary);

  Result BeginSection(BinarySection section_type, Offset size) override;

  Result OnImport(Index index,
                  StringSlice module_name,
                  StringSlice field_name) override;
  Result OnImportFunc(Index import_index,
                      StringSlice module_name,
                      StringSlice field_name,
                      Index func_index,
                      Index sig_index) override;
  Result OnImportGlobal(Index import_index,
                        StringSlice module_name,
                        StringSlice field_name,
                        Index global_index,
                        Type type,
                        bool mutable_) override;

  Result OnFunctionCount(Index count) override;

  Result OnTable(Index index,
                 Type elem_type,
                 const Limits* elem_limits) override;

  Result OnMemory(Index index, const Limits* limits) override;

  Result OnExport(Index index,
                  ExternalKind kind,
                  Index item_index,
                  StringSlice name) override;

  Result OnElemSegmentFunctionIndexCount(Index index,
                                         Index count) override;

  Result BeginDataSegment(Index index, Index memory_index) override;
  Result OnDataSegmentData(Index index,
                           const void* data,
                           Address size) override;

  Result BeginNamesSection(Offset size) override;

  Result OnFunctionName(Index function_index,
                        StringSlice function_name) override;

  Result OnRelocCount(Index count,
                      BinarySection section_code,
                      StringSlice section_name) override;
  Result OnReloc(RelocType type,
                 Offset offset,
                 Index index,
                 uint32_t addend) override;

  Result OnInitExprI32ConstExpr(Index index, uint32_t value) override;

 private:
  LinkerInputBinary* binary;

  Section* reloc_section = nullptr;
  Section* current_section = nullptr;
  Index function_count = 0;
};

BinaryReaderLinker::BinaryReaderLinker(LinkerInputBinary* binary)
    : binary(binary) {}

Result BinaryReaderLinker::OnRelocCount(Index count,
                                        BinarySection section_code,
                                        StringSlice section_name) {
  if (section_code == BinarySection::Custom) {
    WABT_FATAL("relocation for custom sections not yet supported\n");
  }

  for (const std::unique_ptr<Section>& section : binary->sections) {
    if (section->section_code != section_code)
      continue;
    reloc_section = section.get();
    return Result::Ok;
  }

  WABT_FATAL("section not found: %d\n", static_cast<int>(section_code));
  return Result::Error;
}

Result BinaryReaderLinker::OnReloc(RelocType type,
                                   Offset offset,
                                   Index index,
                                   uint32_t addend) {
  if (offset + RELOC_SIZE > reloc_section->size) {
    WABT_FATAL("invalid relocation offset: %#" PRIoffset "\n", offset);
  }

  reloc_section->relocations.emplace_back(type, offset, index, addend);

  return Result::Ok;
}

Result BinaryReaderLinker::OnImport(Index index,
                                    StringSlice module_name,
                                    StringSlice field_name) {
  if (!string_slice_eq_cstr(&module_name, WABT_LINK_MODULE_NAME)) {
    WABT_FATAL("unsupported import module: " PRIstringslice,
               WABT_PRINTF_STRING_SLICE_ARG(module_name));
  }
  return Result::Ok;
}

Result BinaryReaderLinker::OnImportFunc(Index import_index,
                                        StringSlice module_name,
                                        StringSlice field_name,
                                        Index global_index,
                                        Index sig_index) {
  binary->function_imports.emplace_back();
  FunctionImport* import = &binary->function_imports.back();
  import->name = field_name;
  import->sig_index = sig_index;
  import->active = true;
  binary->active_function_imports++;
  return Result::Ok;
}

Result BinaryReaderLinker::OnImportGlobal(Index import_index,
                                          StringSlice module_name,
                                          StringSlice field_name,
                                          Index global_index,
                                          Type type,
                                          bool mutable_) {
  binary->global_imports.emplace_back();
  GlobalImport* import = &binary->global_imports.back();
  import->name = field_name;
  import->type = type;
  import->mutable_ = mutable_;
  binary->active_global_imports++;
  return Result::Ok;
}

Result BinaryReaderLinker::OnFunctionCount(Index count) {
  function_count = count;
  return Result::Ok;
}

Result BinaryReaderLinker::BeginSection(BinarySection section_code,
                                        Offset size) {
  Section* sec = new Section();
  binary->sections.emplace_back(sec);
  current_section = sec;
  sec->section_code = section_code;
  sec->size = size;
  sec->offset = state->offset;
  sec->binary = binary;

  if (sec->section_code != BinarySection::Custom &&
      sec->section_code != BinarySection::Start) {
    size_t bytes_read = read_u32_leb128(
        &binary->data[sec->offset], &binary->data[binary->size], &sec->count);
    if (bytes_read == 0)
      WABT_FATAL("error reading section element count\n");
    sec->payload_offset = sec->offset + bytes_read;
    sec->payload_size = sec->size - bytes_read;
  }
  return Result::Ok;
}

Result BinaryReaderLinker::OnTable(Index index,
                                   Type elem_type,
                                   const Limits* elem_limits) {
  if (elem_limits->has_max && (elem_limits->max != elem_limits->initial))
    WABT_FATAL("Tables with max != initial not supported by wabt-link\n");

  binary->table_elem_count = elem_limits->initial;
  return Result::Ok;
}

Result BinaryReaderLinker::OnElemSegmentFunctionIndexCount(Index index,
                                                           Index count) {
  Section* sec = current_section;

  /* Modify the payload to include only the actual function indexes */
  size_t delta = state->offset - sec->payload_offset;
  sec->payload_offset += delta;
  sec->payload_size -= delta;
  return Result::Ok;
}

Result BinaryReaderLinker::OnMemory(Index index, const Limits* page_limits) {
  Section* sec = current_section;
  sec->data.memory_limits = *page_limits;
  binary->memory_page_count = page_limits->initial;
  return Result::Ok;
}

Result BinaryReaderLinker::BeginDataSegment(Index index, Index memory_index) {
  Section* sec = current_section;
  if (!sec->data.data_segments) {
    sec->data.data_segments = new std::vector<DataSegment>();
  }
  sec->data.data_segments->emplace_back();
  DataSegment& segment = sec->data.data_segments->back();
  segment.memory_index = memory_index;
  return Result::Ok;
}

Result BinaryReaderLinker::OnInitExprI32ConstExpr(Index index, uint32_t value) {
  Section* sec = current_section;
  if (sec->section_code != BinarySection::Data)
    return Result::Ok;
  DataSegment& segment = sec->data.data_segments->back();
  segment.offset = value;
  return Result::Ok;
}

Result BinaryReaderLinker::OnDataSegmentData(Index index,
                                             const void* src_data,
                                             Address size) {
  Section* sec = current_section;
  DataSegment& segment = sec->data.data_segments->back();
  segment.data = static_cast<const uint8_t*>(src_data);
  segment.size = size;
  return Result::Ok;
}

Result BinaryReaderLinker::OnExport(Index index,
                                    ExternalKind kind,
                                    Index item_index,
                                    StringSlice name) {
  binary->exports.emplace_back();
  Export* export_ = &binary->exports.back();
  export_->name = name;
  export_->kind = kind;
  export_->index = item_index;
  return Result::Ok;
}

Result BinaryReaderLinker::BeginNamesSection(Offset size) {
  binary->debug_names.resize(function_count + binary->function_imports.size());
  return Result::Ok;
}

Result BinaryReaderLinker::OnFunctionName(Index index, StringSlice name) {
  binary->debug_names[index] = string_slice_to_string(name);
  return Result::Ok;
}

}  // namespace

Result read_binary_linker(LinkerInputBinary* input_info, LinkOptions* options) {
  BinaryReaderLinker reader(input_info);

  ReadBinaryOptions read_options = WABT_READ_BINARY_OPTIONS_DEFAULT;
  read_options.read_debug_names = true;
  read_options.log_stream = options->log_stream;
  return read_binary(input_info->data, input_info->size, &reader,
                     &read_options);
}

}  // namespace link
}  // namespace wabt
