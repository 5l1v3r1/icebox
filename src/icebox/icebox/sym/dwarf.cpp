#include "sym.hpp"

#define FDP_MODULE "dwarf"
#include "log.hpp"
#include "utils/fnview.hpp"

#include <dwarf.h>
#include <libdwarf.h>

namespace
{
    struct Dwarf
        : public sym::IMod
    {
         Dwarf(fs::path filename);
        ~Dwarf();

        // methods
        bool setup();

        // IModule methods
        span_t              span        () override;
        opt<uint64_t>       symbol      (const std::string& symbol) override;
        opt<uint64_t>       struc_offset(const std::string& struc, const std::string& member) override;
        opt<size_t>         struc_size  (const std::string& struc) override;
        opt<sym::ModCursor> symbol      (uint64_t addr) override;
        bool                sym_list    (sym::on_sym_fn on_sym) override;

        // members
        const fs::path filename_;
        Dwarf_Debug dbg = nullptr;
        Dwarf_Error err = nullptr;
        std::vector<Dwarf_Die> structures; // buffer of all references of structures
    };
}

namespace
{
    static bool open_file(Dwarf& p)
    {
        const auto ok = dwarf_init_path(
            p.filename_.generic_string().data(), // path
            nullptr,                             // true_path_out_buffer
            0,                                   // true_path_bufferlen
            DW_DLC_READ,                         // access
            DW_GROUPNUMBER_ANY,                  // groupnumber
            0,                                   // errhand
            nullptr,                             // errarg
            &p.dbg,                              // ret_dbg
            nullptr,                             // reserved1
            0,                                   // reserved2
            nullptr,                             // reserved3
            &p.err                               // error
        );

        if(ok == DW_DLV_ERROR)
            return FAIL(false, "libdwarf error {} when initializing dwarf file : {}", dwarf_errno(p.err), dwarf_errmsg(p.err));

        if(ok == DW_DLV_NO_ENTRY)
            return FAIL(false, "unfound file or dwarf information in file '{}'", p.filename_.generic_string());

        return true;
    }

    static opt<std::vector<Dwarf_Die>> read_cu(Dwarf& p)
    {
        auto cu = std::vector<Dwarf_Die>();
        Dwarf_Unsigned cu_offset;
        int            ok;

        while(true)
        {
            ok = dwarf_next_cu_header_d(
                p.dbg,      // dbg
                true,       // is_info
                nullptr,    // cu_header_length
                nullptr,    // version_stamp
                nullptr,    // abbrev_offset
                nullptr,    // address_size
                nullptr,    // length_size
                nullptr,    // extension_size
                nullptr,    // type signature
                nullptr,    // typeoffset
                &cu_offset, // next_cu_header_offset
                nullptr,    // header_cu_type
                &p.err      // error
            );

            if(ok == DW_DLV_ERROR)
                return FAIL(ext::nullopt, "libdwarf error {} when reading dwarf file : {}", dwarf_errno(p.err), dwarf_errmsg(p.err));

            if(ok == DW_DLV_NO_ENTRY)
                break;

            Dwarf_Die die = nullptr;
            ok            = dwarf_siblingof_b(p.dbg, 0, true, &die, &p.err);

            if(ok == DW_DLV_NO_ENTRY)
                continue;

            if(ok == DW_DLV_ERROR)
                return FAIL(ext::nullopt, "libdwarf error {} when reading dwarf file : {}", dwarf_errno(p.err), dwarf_errmsg(p.err));

            cu.push_back(die);
        }

        if(cu.empty())
            return FAIL(ext::nullopt, "no compilation unit found in file {}", p.filename_.generic_string());

        return cu;
    }

    static bool read_children(Dwarf& p, const Dwarf_Die& parent, std::vector<Dwarf_Die>& children)
    {
        children.clear();

        Dwarf_Die child = nullptr;
        auto ok         = dwarf_child(parent, &child, &p.err);

        while(ok != DW_DLV_NO_ENTRY)
        {
            if(ok == DW_DLV_ERROR)
            {
                children.clear();
                return FAIL(false, "libdwarf error {} when reading dwarf file : {}", dwarf_errno(p.err), dwarf_errmsg(p.err));
            }
            children.push_back(child);

            ok = dwarf_siblingof_b(p.dbg, child, true, &child, &p.err);
        }

        return true;
    }

    static opt<Dwarf_Die> get_structure(Dwarf& p, const std::string& name, const std::vector<Dwarf_Die>& collection_of_dies, const bool& pass_through_anonymous_struct = false)
    {
        for(const auto structure : collection_of_dies)
        {
            char* name_ptr        = nullptr;
            const auto ok_diename = dwarf_diename(structure, &name_ptr, &p.err);

            if(ok_diename == DW_DLV_ERROR)
                LOG(ERROR, "libdwarf error {} when reading name of a DIE : {}", dwarf_errno(p.err), dwarf_errmsg(p.err));

            if(pass_through_anonymous_struct && ok_diename == DW_DLV_NO_ENTRY) // anonymous structure
            {
                Dwarf_Off type_offset = 0;
                auto ok               = dwarf_dietype_offset(structure, &type_offset, &p.err);

                if(ok == DW_DLV_ERROR)
                    LOG(ERROR, "libdwarf error {} when reading type offset of a DIE : {}", dwarf_errno(p.err), dwarf_errmsg(p.err));

                if(ok != DW_DLV_OK)
                    continue;

                Dwarf_Die anonymous_struct = nullptr;
                ok                         = dwarf_offdie_b(p.dbg, type_offset, true, &anonymous_struct, &p.err);

                if(ok == DW_DLV_ERROR)
                    LOG(ERROR, "libdwarf error {} when getting DIE : {}", dwarf_errno(p.err), dwarf_errmsg(p.err));

                if(ok != DW_DLV_OK)
                {
                    LOG(ERROR, "unable to get DIE at offset {:#x}", type_offset);
                    continue;
                }

                std::vector<Dwarf_Die> children;
                if(!read_children(p, anonymous_struct, children))
                    continue;

                const auto child = get_structure(p, name, children, true);
                if(child)
                    return *child;
            }

            if(ok_diename != DW_DLV_OK)
                continue;

            const std::string structure_name(name_ptr);
            if(structure_name == name)
                return structure;
        }

        LOG(ERROR, "unable to find structure '{}'", name);
        return {};
    }

    static opt<Dwarf_Die> get_structure(Dwarf& p, const std::string& name)
    {
        return get_structure(p, name, p.structures);
    }

    static opt<uint64_t> get_attr_member_location(Dwarf& p, const Dwarf_Die& die)
    {
        Dwarf_Attribute attr = nullptr;
        const auto ok        = dwarf_attr(die, DW_AT_data_member_location, &attr, &p.err);

        if(ok == DW_DLV_ERROR)
            return FAIL(ext::nullopt, "libdwarf error {} when reading attributes of a DIE : {}", dwarf_errno(p.err), dwarf_errmsg(p.err));

        if(ok == DW_DLV_NO_ENTRY)
            return FAIL(ext::nullopt, "die member has not DW_AT_data_member_location attribute");

        Dwarf_Half form;
        if(dwarf_whatform(attr, &form, &p.err) != DW_DLV_OK)
            return FAIL(ext::nullopt, "libdwarf error {} when reading form of DW_AT_data_member_location attribute : {}", dwarf_errno(p.err), dwarf_errmsg(p.err));

        if(form == DW_FORM_data1 || form == DW_FORM_data2 || form == DW_FORM_data4 || form == DW_FORM_data8 || form == DW_FORM_udata)
        {
            Dwarf_Unsigned offset;
            if(dwarf_formudata(attr, &offset, &p.err) != DW_DLV_OK)
                return FAIL(ext::nullopt, "libdwarf error {} when reading DW_AT_data_member_location attribute : {}", dwarf_errno(p.err), dwarf_errmsg(p.err));

            return (uint64_t) offset;
        }
        else if(form == DW_FORM_sdata)
        {
            Dwarf_Signed soffset;
            if(dwarf_formsdata(attr, &soffset, &p.err) != DW_DLV_OK)
                return FAIL(ext::nullopt, "libdwarf error {} when reading DW_AT_data_member_location attribute : {}", dwarf_errno(p.err), dwarf_errmsg(p.err));

            if(soffset < 0)
                return FAIL(ext::nullopt, "unsupported negative offset for DW_AT_data_member_location attribute");

            return (uint64_t) soffset;
        }
        else
        {
            Dwarf_Locdesc** llbuf = nullptr;
            Dwarf_Signed listlen;
            if(dwarf_loclist_n(attr, &llbuf, &listlen, &p.err) != DW_DLV_OK)
            {
                dwarf_dealloc(p.dbg, &llbuf, DW_DLA_LOCDESC);
                return FAIL(ext::nullopt, "unsupported member offset in DW_AT_data_member_location attribute");
            }

            if(listlen != 1 || llbuf[0]->ld_cents != 1 || (llbuf[0]->ld_s[0]).lr_atom != DW_OP_plus_uconst)
            {
                dwarf_dealloc(p.dbg, &llbuf, DW_DLA_LOCDESC);
                return FAIL(ext::nullopt, "unsupported location expression in DW_AT_data_member_location attribute");
            }

            const auto offset = llbuf[0]->ld_s[0].lr_number;
            dwarf_dealloc(p.dbg, &llbuf, DW_DLA_LOCDESC);
            return (uint64_t) offset;
        }
    }

    static opt<size_t> struc_size_internal(Dwarf& p, const Dwarf_Die& struc)
    {
        Dwarf_Unsigned size;
        const auto ok = dwarf_bytesize(struc, &size, &p.err);

        if(ok == DW_DLV_ERROR)
            return FAIL(ext::nullopt, "libdwarf error {} when reading size of a DIE : {}", dwarf_errno(p.err), dwarf_errmsg(p.err));

        if(ok == DW_DLV_NO_ENTRY)
            return FAIL(ext::nullopt, "die has not DW_AT_byte_size attribute");

        return size;
    }
}

Dwarf::Dwarf(fs::path filename)
    : filename_(std::move(filename))
{
}

Dwarf::~Dwarf()
{
    if(dwarf_finish(dbg, &err) != DW_DLV_OK)
    {
        LOG(ERROR, "unable to free dwarf ressources ({}) : {}", dwarf_errno(err), dwarf_errmsg(err));
    }
}

std::unique_ptr<sym::IMod> sym::make_dwarf(span_t /*span*/, const std::string& module, const std::string& guid)
{
    const auto path = getenv("_LINUX_SYMBOL_PATH");
    if(!path)
        return nullptr;

    auto ptr = std::make_unique<Dwarf>(fs::path(path) / module / guid / "elf");
    if(!ptr->setup())
        return nullptr;

    return ptr;
}

std::unique_ptr<sym::IMod> sym::make_dwarf(span_t /*span*/, const void* /*data*/, const size_t /*data_size*/)
{
    LOG(ERROR, "make_dwarf(span, data, data_size) not implemented");
    return nullptr;
}

bool Dwarf::setup()
{
    if(!open_file(*this))
        return false;

    const auto cu = read_cu(*this);
    if(!cu)
        return false;

    std::vector<Dwarf_Die> children;
    for(const auto die : *cu)
        if(read_children(*this, die, children))
            structures.insert(structures.end(), children.begin(), children.end());

    if(structures.empty())
        return FAIL(false, "no structures found in file {}", filename_.generic_string());

    return true;
}

span_t Dwarf::span()
{
    return {};
}

opt<uint64_t> Dwarf::symbol(const std::string& /*symbol*/)
{
    return {};
}

bool Dwarf::sym_list(sym::on_sym_fn /*on_sym*/)
{
    return false;
}

opt<uint64_t> Dwarf::struc_offset(const std::string& struc, const std::string& member)
{
    const auto structure = get_structure(*this, struc);
    if(!structure)
        return {};

    std::vector<Dwarf_Die> children;
    if(!read_children(*this, *structure, children))
        return {};

    const auto child = get_structure(*this, member, children, true);
    if(!child)
        return {};

    const auto offset = get_attr_member_location(*this, *child);
    if(!offset)
        return {};

    return offset;
}

opt<size_t> Dwarf::struc_size(const std::string& struc)
{
    const auto structure = get_structure(*this, struc);
    if(!structure)
        return {};

    return struc_size_internal(*this, *structure);
}

opt<sym::ModCursor> Dwarf::symbol(uint64_t /*addr*/)
{
    return {};
}
