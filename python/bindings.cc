// bindings.cpp
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include <filesystem>


#include "binaryreader.h"
#include "analysis.h"
#include "analysisregister.h"




namespace py = pybind11;


#include <optional>


#include <sstream>

std::string run_analysis_file(const std::string& filepath,
                              const std::string& analysis_name,
                              const std::vector<std::string>& quantities,
                              std::optional<std::string> save_path = std::nullopt,
                              bool print_output = true) {
    auto dispatcher = std::make_shared<DispatchingAccessor>();
    auto analysis = AnalysisRegistry::instance().create(analysis_name);
    if (!analysis) {
        throw std::runtime_error("Unknown analysis '" + analysis_name + "'");
    }

    dispatcher->register_analysis(analysis);
    BinaryReader reader(filepath, quantities, dispatcher);
    reader.read();

    if (save_path) {
        analysis->save(*save_path);
    }

    std::ostringstream output;
    if (print_output) {
        analysis->print_result_to(output);
    }

    return output.str();
}


class CollectorAccessor : public Accessor {
public:
    std::unordered_map<std::string, std::vector<double>> doubles;
    std::unordered_map<std::string, std::vector<int32_t>> ints;
    std::vector<int> event_sizes;
    void on_particle_block(const ParticleBlock& block) override {
        event_sizes.push_back(block.npart);
        for (size_t i = 0; i < block.npart; ++i) {
            for (const auto& [name, info] : quantity_string_map) {
                if (!layout) throw std::runtime_error("Layout not set");

                const auto& particle = block.particles[i];
                Quantity q = info.quantity;

                auto offset_it = layout->find(q);
                if (offset_it == layout->end()) continue;

                size_t offset = offset_it->second;
                if (info.type == QuantityType::Double) {
                    double val;
                    std::memcpy(&val, particle.data() + offset, sizeof(double));
                    doubles[name].push_back(val);
                } else if (info.type == QuantityType::Int32) {
                    int32_t val;
                    std::memcpy(&val, particle.data() + offset, sizeof(int32_t));
                    ints[name].push_back(val);
                }
            }
        }
    }

    const std::vector<double>& get_double_array(const std::string& name) const {
        return doubles.at(name);
    }

    const std::vector<int32_t>& get_int_array(const std::string& name) const {
        return ints.at(name);
    }
    const std::vector<int>& get_event_sizes() const {return event_sizes;}

};



class DictCollectorAccessor : public Accessor {
public:
    std::vector<py::dict> collected_particles;

    void on_particle_block(const ParticleBlock& block) override {
        py::gil_scoped_acquire gil;

        for (size_t i = 0; i < block.npart; ++i) {
            const auto& particle = block.particles[i];
            py::dict d;

            for (const auto& [quantity, offset] : *layout) {
                // Find name and type for this quantity
                auto name_it = std::find_if(
                    quantity_string_map.begin(), quantity_string_map.end(),
                    [quantity](const auto& pair) {
                        return pair.second.quantity == quantity;
                    });

                if (name_it == quantity_string_map.end()) continue;
                const std::string& name = name_it->first;
                const QuantityInfo& info = name_it->second;

                if (info.type == QuantityType::Double) {
                    double val;
                    std::memcpy(&val, particle.data() + offset, sizeof(double));
                    d[py::str(name)] = val;
                } else if (info.type == QuantityType::Int32) {
                    int32_t val;
                    std::memcpy(&val, particle.data() + offset, sizeof(int32_t));
                    d[py::str(name)] = val;
                }
            }

            collected_particles.push_back(std::move(d));
        }
    }

    py::list get_particle_dicts() const {
        py::list l;
        for (const auto& d : collected_particles) {
            l.append(d);
        }
        return l;
    }
};





// Trampoline to call Python overrides
class PyAccessor : public Accessor {
public:
    using Accessor::Accessor;

    void on_particle_block(const ParticleBlock& block) override {
        PYBIND11_OVERRIDE(void, Accessor, on_particle_block, block);
    }

    void on_end_block(const EndBlock& block) override {
        PYBIND11_OVERRIDE(void, Accessor, on_end_block, block);
    }
};

PYBIND11_MODULE(_bindings, m) {



m.def("run_analysis_file", &run_analysis_file,
      py::arg("filepath"),
      py::arg("analysis_name"),
      py::arg("quantities"),
      py::arg("save_path") = std::nullopt,
      py::arg("print_output") = true);

    py::class_<ParticleBlock>(m, "ParticleBlock")
        .def_readonly("event_number", &ParticleBlock::event_number)
        .def_readonly("ensamble_number", &ParticleBlock::ensamble_number)
        .def_readonly("npart", &ParticleBlock::npart);

    py::class_<EndBlock>(m, "EndBlock")
        .def_readonly("event_number", &EndBlock::event_number)
        .def_readonly("impact_parameter", &EndBlock::impact_parameter);

    py::class_<Accessor, PyAccessor, std::shared_ptr<Accessor>>(m, "Accessor")
        .def(py::init<>())
        .def("on_particle_block", &Accessor::on_particle_block)
        .def("on_end_block", &Accessor::on_end_block)
        .def("get_int", &Accessor::get_int)
        .def("get_double", &Accessor::get_double);
    py::class_<BinaryReader>(m, "BinaryReader")
        .def(py::init<const std::string&, const std::vector<std::string>&, std::shared_ptr<Accessor>>())
        .def("read", &BinaryReader::read);

  py::class_<DictCollectorAccessor, Accessor, std::shared_ptr<DictCollectorAccessor>>(m, "DictCollectorAccessor")
    .def(py::init<>())
    .def("get_particle_dicts", &DictCollectorAccessor::get_particle_dicts);

    py::class_<CollectorAccessor, Accessor, std::shared_ptr<CollectorAccessor>>(m, "CollectorAccessor")
        .def(py::init<>())
        .def("get_double_array", [](const CollectorAccessor& self, const std::string& name) {
            const auto& vec = self.get_double_array(name);
            return py::array(vec.size(), vec.data());
        })
        .def("get_event_sizes", [](const CollectorAccessor& self) {
            const auto& vec = self.get_event_sizes();
            return py::array(vec.size(), vec.data());
        })

        .def("get_int_array", [](const CollectorAccessor& self, const std::string& name) {
            const auto& vec = self.get_int_array(name);
            return py::array(vec.size(), vec.data());
        });

}
