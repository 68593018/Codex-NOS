import sys
import os
import re

try:
    import yaml
    USE_PYYAML = True
except ImportError:
    USE_PYYAML = False

def to_c_macro(name):
    return re.sub(r'[^a-zA-Z0-9]', '_', name).upper()

def parse_yaml_fallback(file_path):
    data = {"nodes": [], "components": [], "profiles": [], "models": [], "platform": {"provides": []}}
    if not os.path.exists(file_path): return data
    with open(file_path, 'r') as f:
        lines = f.readlines()
    mode = None
    for line in lines:
        line = line.rstrip()
        if not line or line.lstrip().startswith('#'): continue
        indent = len(line) - len(line.lstrip())
        content = line.strip()
        if content == "nodes:": mode = "nodes"; continue
        if content == "services:": mode = "services"; continue
        if content == "components:": mode = "components"; continue
        if content == "profiles:": mode = "profiles"; continue
        if content == "models:": mode = "models"; continue
        if content == "platform:": mode = "platform"; continue
        
        if mode == "nodes":
            if content.startswith("- name:"):
                data["nodes"].append({"name": content.split(":")[1].strip().strip('"'), "threads": [], "buffer_profile": "default"})
            elif "uds_path:" in content:
                data["nodes"][-1]["uds_path"] = content.split(":")[1].strip().strip('"')
            elif "buffer_profile:" in content:
                data["nodes"][-1]["buffer_profile"] = content.split(":")[1].strip().strip('"')
            elif "busy_poll_cycles:" in content:
                data["nodes"][-1]["busy_poll_cycles"] = int(content.split(":")[1].strip())
            elif content.startswith("- name:") and indent > 4:
                data["nodes"][-1]["threads"].append({"name": content.split(":")[1].strip().strip('"'), "components": []})
            elif "components:" in content:
                comps = content.split(":")[1].strip().strip('[]').split(',')
                data["nodes"][-1]["threads"][-1]["components"] = [c.strip().strip('"') for c in comps if c.strip()]
        elif mode == "components":
            if content.startswith("- name:"):
                data["components"].append({"name": content.split(":")[1].strip().strip('"'), "model": "", "provides": [], "requires": []})
            elif "id:" in content and indent == 4:
                data["components"][-1]["id"] = int(content.split(":")[1].strip())
            elif "model:" in content:
                data["components"][-1]["model"] = content.split(":")[1].strip().strip('"')
            elif "provides:" in content:
                pass
            elif content.startswith("- { name:") and indent > 4:
                svc_name = re.search(r'name:\s*"([^"]+)"', content).group(1)
                svc_id = int(re.search(r'id:\s*(\d+)', content).group(1))
                data["components"][-1]["provides"].append({"name": svc_name, "id": svc_id})
            elif "requires:" in content:
                pass
            elif content.startswith("- ") and indent > 4:
                req_name = content.split("-")[1].strip().strip('"')
                data["components"][-1]["requires"].append(req_name)
        elif mode == "platform":
            if content.startswith("- { name:") and indent > 4:
                svc_name = re.search(r'name:\s*"([^"]+)"', content).group(1)
                svc_id = int(re.search(r'id:\s*(\d+)', content).group(1))
                data["platform"]["provides"].append({"name": svc_name, "id": svc_id, "type": "embedded"})
        elif mode == "models":
            if content.startswith("- name:"):
                data["models"].append({"name": content.split(":")[1].strip().strip('"'), "lib": ""})
            elif "lib:" in content:
                data["models"][-1]["lib"] = content.split(":")[1].strip().strip('"')
        elif mode == "profiles":
            if content.startswith("- name:"):
                data["profiles"].append({"name": content.split(":")[1].strip().strip('"'), "bins": []})
            elif "size:" in content:
                parts = content.strip().strip('{}').split(',')
                bin_data = {}
                for p in parts:
                    k, v = p.split(':')
                    bin_data[k.strip()] = int(v.strip())
                data["profiles"][-1]["bins"].append(bin_data)
    return data

INFRA_INIT_MAP = {"SVC_LOG": "nos_log_init", "SVC_KV_DB": "nos_kv_db_init", "SVC_TIMER": "nos_timer_init"}

class ConfigError(Exception):
    pass

def _item_name(item, field, owner):
    value = item.get(field)
    if value is None or value == "":
        raise ConfigError(f"{owner} missing required field '{field}'")
    return value

def _ensure_unique(items, field, owner):
    seen = {}
    for item in items:
        value = _item_name(item, field, owner)
        if value in seen:
            raise ConfigError(f"duplicate {owner} {field}: {value}")
        seen[value] = item
    return seen

def _validate_service_defs(services, owner, svc_name_to_owner, svc_id_to_owner):
    for svc in services:
        name = _item_name(svc, "name", owner)
        sid = svc.get("id")
        if not isinstance(sid, int) or sid <= 0:
            raise ConfigError(f"{owner} service '{name}' has invalid id: {sid}")
        stype = svc.get("type")
        if stype not in ("embedded", "remote"):
            raise ConfigError(f"{owner} service '{name}' has invalid type: {stype}")
        if name in svc_name_to_owner:
            raise ConfigError(f"duplicate service name '{name}' in {owner}; already provided by {svc_name_to_owner[name]}")
        if sid in svc_id_to_owner:
            raise ConfigError(f"duplicate service id '{sid}' in {owner}; already used by {svc_id_to_owner[sid]}")
        svc_name_to_owner[name] = owner
        svc_id_to_owner[sid] = owner

def validate_config(data):
    nodes = data.get("nodes", [])
    components = data.get("components", [])
    profiles = data.get("profiles", [])
    models = data.get("models", [])
    platform = data.get("platform", {})

    if not nodes:
        raise ConfigError("no nodes configured")
    if not components:
        raise ConfigError("no components configured")
    if not models:
        raise ConfigError("no component models configured")

    comp_map = _ensure_unique(components, "name", "component")
    model_map = _ensure_unique(models, "name", "model")
    profile_map = _ensure_unique(profiles, "name", "profile")

    comp_ids = {}
    svc_name_to_owner = {}
    svc_id_to_owner = {}

    _validate_service_defs(platform.get("provides", []), "platform", svc_name_to_owner, svc_id_to_owner)

    for model in models:
        lib = _item_name(model, "lib", f"model '{model['name']}'")
        if not lib.endswith(".so"):
            raise ConfigError(f"model '{model['name']}' lib should be a shared object: {lib}")

    for profile in profiles:
        bins = profile.get("bins", [])
        if not bins:
            raise ConfigError(f"profile '{profile['name']}' has no bins")
        for idx, bin_def in enumerate(bins):
            size = bin_def.get("size")
            count = bin_def.get("count")
            if not isinstance(size, int) or size <= 0:
                raise ConfigError(f"profile '{profile['name']}' bin {idx} has invalid size: {size}")
            if not isinstance(count, int) or count <= 0:
                raise ConfigError(f"profile '{profile['name']}' bin {idx} has invalid count: {count}")

    for comp in components:
        cname = comp["name"]
        cid = comp.get("id")
        if not isinstance(cid, int) or cid <= 0:
            raise ConfigError(f"component '{cname}' has invalid id: {cid}")
        if cid in comp_ids:
            raise ConfigError(f"duplicate component id '{cid}' in component '{cname}'; already used by '{comp_ids[cid]}'")
        comp_ids[cid] = cname
        model = _item_name(comp, "model", f"component '{cname}'")
        if model not in model_map:
            raise ConfigError(f"component '{cname}' references unknown model '{model}'")
        _validate_service_defs(comp.get("provides", []), f"component '{cname}'", svc_name_to_owner, svc_id_to_owner)

    for comp in components:
        for required in comp.get("requires", []):
            if required not in svc_name_to_owner:
                raise ConfigError(f"component '{comp['name']}' requires unknown service '{required}'")

    node_map = _ensure_unique(nodes, "name", "node")
    for node in nodes:
        nname = node["name"]
        _item_name(node, "uds_path", f"node '{nname}'")
        profile = node.get("buffer_profile", "default")
        if profile not in profile_map:
            raise ConfigError(f"node '{nname}' references unknown buffer_profile '{profile}'")
        threads = node.get("threads", [])
        if not threads:
            raise ConfigError(f"node '{nname}' has no threads")
        thread_names = set()
        for thread in threads:
            tname = _item_name(thread, "name", f"node '{nname}' thread")
            if tname in thread_names:
                raise ConfigError(f"node '{nname}' has duplicate thread '{tname}'")
            thread_names.add(tname)
            comps = thread.get("components", [])
            if not comps:
                raise ConfigError(f"node '{nname}' thread '{tname}' has no components")
            for comp_name in comps:
                if comp_name not in comp_map:
                    raise ConfigError(f"node '{nname}' thread '{tname}' references unknown component '{comp_name}'")
        busy_poll = node.get("busy_poll_cycles", 500)
        if not isinstance(busy_poll, int) or busy_poll < 0:
            raise ConfigError(f"node '{nname}' has invalid busy_poll_cycles: {busy_poll}")

    return comp_map, model_map, profile_map, node_map, svc_name_to_owner

def validate_and_resolve(data):
    validate_config(data)
    comp_map = {c["name"]: c for c in data["components"]}
    model_to_lib = {m["name"]: m["lib"] for m in data["models"]}
    global_svc_map, svc_name_to_id, comp_to_node = {}, {}, {}
    node_to_uds = {n["name"]: n["uds_path"] for n in data["nodes"]}

    for svc in data.get("platform", {}).get("provides", []):
        global_svc_map[svc["name"]] = {"name": svc["name"], "id": svc["id"], "type": "embedded", "node": "Platform"}
        svc_name_to_id[svc["name"]] = svc["id"]

    for node in data["nodes"]:
        for thread in node["threads"]:
            for comp_name in thread["components"]:
                if comp_name in comp_map: comp_to_node[comp_name] = node["name"]

    for comp in data["components"]:
        for svc in comp.get("provides", []):
            sname = svc["name"]
            global_svc_map[sname] = {
                "name": sname, "id": svc["id"], "type": "remote", "provider_id": comp["id"],
                "node": comp_to_node.get(comp["name"], "Unknown"),
                "uds_path": node_to_uds.get(comp_to_node.get(comp["name"], ""), "")
            }
            svc_name_to_id[sname] = svc["id"]

    profile_map = {p["name"]: p["bins"] for p in data["profiles"]}
    for node in data["nodes"]:
        pname = node.get("buffer_profile", "default")
        node["resolved_bins"] = profile_map.get(pname, profile_map.get("default", []))
        node_comps = []
        for thread in node["threads"]:
            res_ids, res_names, res_libs = [], [], []
            for comp_name in thread["components"]:
                comp = comp_map.get(comp_name)
                res_ids.append(comp["id"]); res_names.append(comp_name); res_libs.append(model_to_lib.get(comp["model"]))
                node_comps.append(comp_name)
            thread["comp_ids"] = res_ids; thread["comp_names"] = res_names; thread["comp_libs"] = res_libs

        needed_svc_names = set()
        for comp_name in node_comps:
            comp = comp_map[comp_name]
            for s in comp.get("provides", []): needed_svc_names.add(s["name"])
            for s in comp.get("requires", []): needed_svc_names.add(s)
        
        node["resolved_services"] = sorted(
            [global_svc_map[s] for s in needed_svc_names if s in global_svc_map],
            key=lambda svc: svc["id"]
        )
        node["platform_inits"] = [
            init_func for svc_name, init_func in INFRA_INIT_MAP.items()
            if svc_name in needed_svc_names
        ]

    return {c["name"]: c["id"] for c in data["components"]}, svc_name_to_id

def generate_header(comp_ids, svc_ids, header_path):
    lines = ["#ifndef __NOS_IDS_H__", "#define __NOS_IDS_H__", "", "/* Component IDs */"]
    for name, cid in sorted(comp_ids.items(), key=lambda x: x[1]): lines.append(f"#define {to_c_macro(name):20} {cid}")
    lines.append("\n/* Service IDs */")
    for name, sid in sorted(svc_ids.items(), key=lambda x: x[1]): lines.append(f"#define {to_c_macro(name):20} {sid}")
    lines.append("\n#endif")
    with open(header_path, 'w') as f: f.write("\n".join(lines))

def generate_node_manifest(node, output_path):
    c_content = ['#include <string.h>', '#include "nos_manifest.h"', '']
    for init_func in node["platform_inits"]: c_content.append(f'extern void {init_func}(void);')
    c_content.append('')
    c_content.append(f'static const nos_buffer_pool_def_t g_local_pools[] = {{')
    for b in node["resolved_bins"]: c_content.append(f'    {{ .chunk_size = {b["size"]}, .chunk_count = {b["count"]} }},')
    c_content.append('    { .chunk_size = 0 }\n};')
    c_content.append(f'static const nos_service_def_t g_local_services[] = {{')
    for s in node["resolved_services"]:
        c_content.append(f'    {{ .service_name = "{s["name"]}", .service_id = {s["id"]}, .node_name = "{s["node"]}", .provider_comp_id = {s.get("provider_id", 0)}, .remote_uds_path = "{s.get("uds_path", "")}" }},')
    c_content.append(f'    {{ .service_name = NULL, .service_id = 0 }}\n}};')
    inits_str = " ,".join(node["platform_inits"]) if node["platform_inits"] else ""
    c_content.append(f'static const nos_platform_init_func_t g_infra_inits[] = {{ {inits_str}{" ," if inits_str else ""} NULL }};')
    c_content.append('const nos_node_def_t g_local_node_def = {')
    c_content.append(f'    .name = "{node["name"]}", .uds_path = "{node["uds_path"]}", .buffer_pools = g_local_pools, .busy_poll_cycles = {node.get("busy_poll_cycles", 500)},')
    c_content.append('    .threads = {')
    for thread in node["threads"]:
        ids, names, libs = ", ".join(map(str, thread["comp_ids"])), ", ".join(f'"{n}"' for n in thread["comp_names"]), ", ".join(f'"{l}"' for l in thread["comp_libs"])
        c_content.append(f'        {{ .name = "{thread["name"]}", .comp_ids = {{{ids}, 0}}, .comp_names = {{{names}, NULL}}, .comp_models = {{{libs}, NULL}} }},')
    c_content.append('        { .name = NULL }\n    },')
    c_content.append(f'    .services = g_local_services, .service_count = {len(node["resolved_services"])}, .platform_inits = g_infra_inits\n}};')
    c_content.append('\nconst nos_node_def_t* nos_manifest_get_local(void) { return &g_local_node_def; }')
    with open(output_path, 'w') as f: f.write("\n".join(c_content))

if __name__ == "__main__":
    if len(sys.argv) < 3: sys.exit(1)
    input_dir, out_h = sys.argv[1], sys.argv[2]
    master = {"nodes": [], "components": [], "profiles": [], "models": [], "platform": {"provides": []}}
    for r, ds, files in os.walk(input_dir):
        for f in files:
            if f.endswith(".yaml"):
                if USE_PYYAML:
                    with open(os.path.join(r,f)) as y:
                        fd = yaml.safe_load(y)
                        if fd and "platform" in fd: master["platform"]["provides"].extend(fd["platform"].get("provides", []))
                        if fd:
                            for k in ["nodes", "components", "profiles", "models"]:
                                if k in fd: master[k].extend(fd[k])
                else:
                    fd = parse_yaml_fallback(os.path.join(r,f))
                    if fd:
                        master["platform"]["provides"].extend(fd["platform"]["provides"])
                        for k in ["nodes", "components", "profiles", "models"]: master[k].extend(fd[k])
    try:
        comp_ids, svc_ids = validate_and_resolve(master)
        generate_header(comp_ids, svc_ids, out_h)
        for node in master["nodes"]:
            out_c = os.path.join(os.path.dirname(out_h), f"manifest_{node['name']}.c").replace("include", "src/core")
            generate_node_manifest(node, out_c)
            print(node["name"], end=" ")
    except ConfigError as exc:
        print(f"Configuration error: {exc}", file=sys.stderr)
        sys.exit(2)
