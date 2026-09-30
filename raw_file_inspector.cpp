#include <iostream>
#include <CLI/CLI.hpp>
#include <metavision/sdk/stream/camera.h>

using namespace Metavision;

int main(int argc, char **argv) {
    CLI::App app{"raw_file_inspector"};

    std::string input_file;
    app.add_option("-i,--input", input_file, "Input raw file")->required();

    CLI11_PARSE(app, argc, argv);

    Camera camera;
    FileConfigHints hints;
    camera = Camera::from_file(input_file, hints);

    const auto& config = camera.get_camera_configuration();
    std::cout << "Serial number       : " << config.serial_number << std::endl;
    std::cout << "Plugin name         : " << config.plugin_name << std::endl;
    std::cout << "Integrator          : " << config.integrator << std::endl;
    std::cout << "Data encoding format: " << config.data_encoding_format << std::endl;
    std::cout << "Firmware verion     : " << config.firmware_version << std::endl;

    return 0;
}