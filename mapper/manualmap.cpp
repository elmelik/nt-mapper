#include "stdafx.h"
#include "memory_section.hpp"
#include "api_set.hpp"

bool injection::manualmap::inject(std::vector<uint8_t>& buffer)
{
	logger::log("manualmap initiated");

	// GET LINKED MODULES FOR LATER USE
	this->linked_modules = this->process.get_modules();

	// INITIALISE CONTEXT
	map_ctx ctx("Main Image", buffer);
	ctx.pe.parse();

	// MAP MAIN IMAGE AND ALL DEPENDENCIES
	map_image(ctx);

	// CALL DEPENCY ENTRYPOINTS AND MAIN IMAGE ENTRYPOINTS
	call_entrypoint(ctx);

	return false;
}

bool injection::manualmap::map_image(map_ctx& ctx)
{
	auto section = memory_section(PAGE_EXECUTE_READWRITE, ctx.pe.get_optional_header().SizeOfImage);
	
	if (!section)
		return false; // FAILED TO CREATE SECTION - SHOULDN'T HAPPEN

	// MAP SECTION INTO BOTH LOCAL AND REMOTE PROCESS
	ctx.local_image = process::current_process().map(section);
	ctx.remote_image = this->process.map(section);
	
	// ADD MAPPED MODULE TO LIST OF MODULES
	this->mapped_modules.push_back(ctx);

	// MANUALMAP IMAGE
	write_headers(ctx);
	write_image_sections(ctx);
	fix_import_table(ctx);
	relocate_image_by_delta(ctx);

	return true;
}

uintptr_t injection::manualmap::find_or_map_dependecy(std::string image_name)
{
	// HAVE WE MAPPED THIS MODULE ALREADY?
	for (auto module : this->mapped_modules)
		if (module.image_name == image_name)
			return module.remote_image;

	// WAS THIS MODULE ALREADY LOADED BY LDR?
	if (this->linked_modules.find(image_name) != this->linked_modules.end())
		return this->linked_modules.at(image_name);

	// TODO:
	// PROPER FILE SEARCHING
	auto ctx = map_ctx(image_name, binary_file::read_file("C:\\Windows\\System32\\" + image_name));

	if (map_image(ctx))
		return ctx.remote_image;

	return 0;
}

void injection::manualmap::write_headers(map_ctx& ctx)
{
	memcpy(ctx.local_image_void, ctx.get_pe_buffer(), ctx.pe.get_optional_header().SizeOfHeaders);
}
void injection::manualmap::write_image_sections(map_ctx& ctx)
{
	for (auto section : ctx.pe.get_sections())
		memcpy(reinterpret_cast<void*>(ctx.local_image + section.VirtualAddress), ctx.get_pe_buffer() + section.PointerToRawData, section.SizeOfRawData);
}

void injection::manualmap::call_entrypoint(map_ctx& ctx)
{
	// dllmain_call_x64.asm
	uint8_t shellcode[] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x4D, 0x31, 0xC0, 0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0xC3 };

	*PPTR(shellcode + 0x6) = ctx.remote_image;
	*PPTR(shellcode + 0x1A) = ctx.remote_image + ctx.pe.get_optional_header().AddressOfEntryPoint;

	auto remote_buffer = this->process.raw_allocate(sizeof(shellcode));
	this->process.write_raw_memory(shellcode, sizeof(shellcode), remote_buffer);

	this->process.create_thread(remote_buffer, NULL);
}

void injection::manualmap::relocate_image_by_delta(map_ctx& ctx)
{
	auto delta = ctx.remote_image - ctx.pe.get_image_base();
	
	for (auto& [entry, item] : ctx.pe.get_relocations(ctx.local_image))
		*PPTR(ctx.local_image + entry.page_rva + item.get_offset()) += delta;
}

void injection::manualmap::fix_import_table(map_ctx& ctx)
{
	wstring_converter converter;
	api_set api_schema;

	for (auto& [tmp_name, functions] : ctx.pe.get_imports(ctx.local_image))
	{
		auto module_name = tmp_name; // COMPILER COMPLAINED ABOUT tmp_name BEING CONST??

		std::wstring wide_module_name = converter.from_bytes(module_name.c_str());
		if (api_schema.query(wide_module_name))
			module_name = converter.to_bytes(wide_module_name);

		// TODO: Parse exports
		auto loc_mod_handle = LoadLibraryA(module_name.c_str());
		auto rem_mod_handle = find_or_map_dependecy(module_name);

		if (!rem_mod_handle)
			logger::log_error("Failed to map depedency");

		for (const auto& fn : functions)
		{
			auto rva = PTR(GetProcAddress(loc_mod_handle, fn.name.c_str())) - PTR(loc_mod_handle);
			*PPTR(ctx.local_image + fn.function_rva) = rem_mod_handle + rva;
		}
	}
}

uint8_t* map_ctx::get_pe_buffer()
{
	return this->pe.get_buffer().data();
}
