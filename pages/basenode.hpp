#include "pager.hpp"
#include "page.hpp"
#include "page_header.hpp"


class BaseNode {
    protected:
        Page* page;
        PageHeader* header;
    
    public:
        BaseNode(Page* p): page(p) {
            header = reinterpret_cast<PageHeader*>(p->data);
        }

        // Public Getters/Setters
        void set_type(uint8_t type) { header->node_type = type; }
        uint8_t get_type() { return header->node_type; }

        void set_key_count(uint32_t count) { header->key_count = count; }
        uint32_t get_key_count() { return header->key_count; }

        // Allow children to access the header directly if needed
        PageHeader* get_header() { return header; }
};