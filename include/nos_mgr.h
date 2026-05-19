#ifndef __NOS_MGR_H__
#define __NOS_MGR_H__

typedef struct {
    const char *name;
    const char *binary;
    const char *uds_path;
} nos_mgr_node_def_t;

const nos_mgr_node_def_t* nos_mgr_manifest_get_nodes(void);

#endif /* __NOS_MGR_H__ */
