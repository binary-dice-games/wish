#include <iostream>
#include <stdexcept>
#include <cstring>
#include <yaml.h>

int main() {
    const char* yaml_str = "\ntype: Label\ntext: Hi\nvisible: false\n";
    
    yaml_parser_t parser{};
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_string(&parser,
        reinterpret_cast<const unsigned char*>(yaml_str), strlen(yaml_str));
    
    int event_count = 0;
    while (true) {
        yaml_event_t ev{};
        if (!yaml_parser_parse(&parser, &ev)) { std::cout << "PARSE ERROR\n"; break; }
        std::cout << "Event " << ++event_count << ": type=" << ev.type;
        if (ev.type == YAML_SCALAR_EVENT) {
            std::cout << " value=[" 
                      << reinterpret_cast<const char*>(ev.data.scalar.value)
                      << "] plain=" << (int)ev.data.scalar.style;
        }
        std::cout << "\n";
        bool done = (ev.type == YAML_STREAM_END_EVENT);
        yaml_event_delete(&ev);
        if (done) break;
    }
    yaml_parser_delete(&parser);
    return 0;
}
