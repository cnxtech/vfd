/*
**
** az
**
*/


#include "sriov.h"

#define DEBUG

struct rte_port *ports;

 

static inline uint64_t RDTSC(void)
{
  unsigned int hi, lo;
  __asm__ volatile("rdtsc" : "=a" (lo), "=d" (hi));
  return ((uint64_t)hi << 32) | lo;
}




static void 
sig_int(int sig)
{
  terminated = 1;
  restart = 0;

  int portid;
  if (sig == SIGINT)  {
		for (portid = 0; portid < n_ports; portid++) {
			rte_eth_dev_close(portid);
		}
	}

  static int called = 0;
  
  if(sig == 1) called = sig;

  if(called) 
    return; 
  else called = 1;
  
  traceLog(TRACE_NORMAL, "Received Interrupt signal\n");
}



static void 
sig_usr(int sig)
{
  terminated = 1;
  restart = 1;
    
  static int called = 0;
  
  if(sig == 1) called = sig;

  if(called) 
    return; 
  else called = 1;
  
  traceLog(TRACE_NORMAL, "Restarting sriovctl");
}


static void 
sig_hup(int __attribute__((__unused__)) sig)
{
  restart = 1;
  
  int res = readConfigFile(fname);
  if (res != 0) {
    traceLog(TRACE_ERROR, "Can not read config file: %s\n", fname);
  }
  
  res = update_ports_config();
  if (res != 0) {
    traceLog(TRACE_ERROR, "Error updating ports configuration: %s\n", res);
  }
  
  traceLog(TRACE_NORMAL, "Received HUP signal\n");
}



// Time difference in millisecond

double 
timeDelta(struct timeval * now, struct timeval * before)
{
  time_t delta_seconds;
  time_t delta_microseconds;

  //compute delta in second, 1/10's and 1/1000's second units

  delta_seconds      = now -> tv_sec  - before -> tv_sec;
  delta_microseconds = now -> tv_usec - before -> tv_usec;

  if(delta_microseconds < 0){
    // manually carry a one from the seconds field
    delta_microseconds += 1000000;  // 1e6 
    -- delta_seconds;
  }
  return((double)(delta_seconds * 1000) + (double)delta_microseconds/1000);
}



void
restore_vf_setings(uint8_t port_id, int vf_id)
{
  
  dump_sriov_config(running_config);
  int i;
  int on = 1;
 
  for (i = 0; i < running_config.num_ports; ++i){
    struct sriov_port_s *port = &running_config.ports[i];
    
    if (port_id == port->rte_port_number){
      traceLog(TRACE_DEBUG, "------------------ PORT ID: %d --------------------\n", port->rte_port_number);
      traceLog(TRACE_DEBUG, "------------------ PORT PCIID: %s --------------------\n", port->pciid);
      
      int y;
      for(y = 0; y < port->num_vfs; ++y){
        
        struct vf_s *vf = &port->vfs[y];   
        
        traceLog(TRACE_DEBUG, "------------------ CHECKING VF ID: %d --------------------\n", vf->num);
        
        if(vf_id == vf->num){
           
          uint32_t vf_mask = VFN2MASK(vf->num); 

          traceLog(TRACE_DEBUG, "------------------ DELETING VLANS, VF: %d --------------------\n", vf->num);
          
          int v;
          for(v = 0; v < vf->num_vlans; ++v) {
            int vlan = vf->vlans[v];
            traceLog(TRACE_DEBUG, "------------------ DELETING VLAN: %d, VF: %d --------------------\n", vlan, vf->num );
            set_vf_rx_vlan(port->rte_port_number, vlan, vf_mask, 0);
          }
        
          // add new vlans from config fil
          traceLog(TRACE_DEBUG, "------------------ ADDING VLANS, VF: %d --------------------\n", vf->num);
          
          for(v = 0; v < vf->num_vlans; ++v) {
            int vlan = vf->vlans[v];
            traceLog(TRACE_DEBUG, "------------------ ADDIND VLAN: %d, VF: %d --------------------\n", vlan, vf->num );
            set_vf_rx_vlan(port->rte_port_number, vlan, vf_mask, on);
          }
          
          // set VLAN anti spoofing when VLAN filter is used
          set_vf_vlan_anti_spoofing(port->rte_port_number, vf->num, vf->vlan_anti_spoof);         
          set_vf_mac_anti_spoofing(port->rte_port_number, vf->num, vf->mac_anti_spoof);

                 
          rx_vlan_strip_set_on_vf(port->rte_port_number, vf->num, vf->strip_stag);         
          rx_vlan_insert_set_on_vf(port->rte_port_number, vf->num, vf->insert_stag);
                 
          
         
          traceLog(TRACE_DEBUG, "------------------ DELETING MACs, VF: %d --------------------\n", vf->num);
          
          int m;
          for(m = 0; m < vf->num_macs; ++m) {
            __attribute__((__unused__)) char *mac = vf->macs[m];
            set_vf_rx_mac(port->rte_port_number, mac, vf->num, 0);
          }

          traceLog(TRACE_DEBUG, "------------------ ADDING MACs, VF: %d --------------------\n", vf->num);

          // iterate through all macs
          for(m = 0; m < vf->num_macs; ++m) {
            __attribute__((__unused__)) char *mac = vf->macs[m];
            set_vf_rx_mac(port->rte_port_number, mac, vf->num, 1);
          }
                 
          set_vf_allow_bcast(port->rte_port_number, vf->num, vf->allow_bcast);
          set_vf_allow_mcast(port->rte_port_number, vf->num, vf->allow_mcast);
          set_vf_allow_un_ucast(port->rte_port_number, vf->num, vf->allow_un_ucast);            
        }
      }
    }      
  }
     
}


int
update_ports_config(void)
{
  int i;
  int on = 1;
 
  for (i = 0; i < sriov_config.num_ports; ++i){
    
    int ret;
  
    struct sriov_port_s *port = &sriov_config.ports[i];
    
    // running config
    struct sriov_port_s *r_port = &running_config.ports[i];
    
    // if running config older then new config update settings
    if (r_port->last_updated < port->last_updated) {
      traceLog(TRACE_DEBUG, "------------------ UPDADING PORT: %d, r_port time: %d, c_port time: %d, --------------------\n",
              i, r_port->last_updated, port->last_updated);
      
      rte_eth_promiscuous_enable(port->rte_port_number);
      rte_eth_allmulticast_enable(port->rte_port_number);
      
      ret = rte_eth_dev_uc_all_hash_table_set(port->rte_port_number, on);
      if (ret < 0)
        traceLog(TRACE_ERROR, "bad unicast hash table parameter, return code = %d \n", ret);
      
      r_port->rte_port_number = port->rte_port_number;
      r_port->last_updated = port->last_updated;
      strcpy(r_port->name, port->name);
      strcpy(r_port->pciid, port->pciid);
      r_port->last_updated = port->last_updated;
      r_port->mtu = port->mtu;
      r_port->num_mirros= port->num_mirros;
      r_port->num_vfs = port->num_vfs;
    }
     
    
    /* go through all VF's and set VLAN's */
    uint32_t vf_mask;
    
    int y;
    for(y = 0; y < port->num_vfs; ++y){
      
      struct vf_s *vf = &port->vfs[y];   
      
      // running VF's
      struct vf_s *r_vf = &r_port->vfs[y];
      vf_mask = VFN2MASK(vf->num);    

      if(r_vf->last_updated == 0)
        r_vf->num = vf->num;
      
      traceLog(TRACE_DEBUG, "HERE WE ARE = %d, vf->num %d, r_vf->num: %d, vf->last_updated: %d, r_vf->last_updated: %d\n", 
      y, vf->num, r_vf->num, vf->last_updated, r_vf->last_updated);      
      
      int v;
      // delete running vlans
      if (vf->num == r_vf->num && vf->last_updated > r_vf->last_updated) {
        traceLog(TRACE_DEBUG, "------------------ DELETING VLANS, VF: %d --------------------\n", vf->num);
        
        for(v = 0; v < r_vf->num_vlans; ++v) {
          int vlan = r_vf->vlans[v];
          set_vf_rx_vlan(port->rte_port_number, vlan, vf_mask, 0);
        }
      
        // add new vlans from config file

        traceLog(TRACE_DEBUG, "------------------ ADDING VLANS, VF: %d --------------------\n", vf->num);
        int v;
        for(v = 0; v < vf->num_vlans; ++v) {
          int vlan = vf->vlans[v];
          set_vf_rx_vlan(port->rte_port_number, vlan, vf_mask, on);
          
          // update running config
          r_vf->vlans[v] = vlan;
        }
        r_vf->num_vlans = vf->num_vlans;
        

           
        traceLog(TRACE_DEBUG, "------------------ DELETING MACs, VF: %d --------------------\n", vf->num);
        
        int m;
        for(m = 0; m < r_vf->num_macs; ++m) {
          __attribute__((__unused__)) char *mac = vf->macs[m];
          set_vf_rx_mac(port->rte_port_number, mac, vf->num, 0);
        }

        traceLog(TRACE_DEBUG, "------------------ ADDING MACs, VF: %d --------------------\n", vf->num);

        // iterate through all macs
        for(m = 0; m < vf->num_macs; ++m) {
          __attribute__((__unused__)) char *mac = vf->macs[m];
          set_vf_rx_mac(port->rte_port_number, mac, vf->num, 1);
        }
        
        
 

        // set VLAN anti spoofing when VLAN filter is used
        set_vf_vlan_anti_spoofing(port->rte_port_number, vf->num, vf->vlan_anti_spoof);         
        set_vf_mac_anti_spoofing(port->rte_port_number, vf->num, vf->mac_anti_spoof);

               
        rx_vlan_strip_set_on_vf(port->rte_port_number, vf->num, vf->strip_stag);       
        rx_vlan_insert_set_on_vf(port->rte_port_number, vf->num, vf->insert_stag);
        
        set_vf_allow_bcast(port->rte_port_number, vf->num, vf->allow_bcast);
        set_vf_allow_mcast(port->rte_port_number, vf->num, vf->allow_mcast);
        set_vf_allow_un_ucast(port->rte_port_number, vf->num, vf->allow_un_ucast); 
        
        r_vf->vlan_anti_spoof = vf->vlan_anti_spoof;
        r_vf->mac_anti_spoof  = vf->mac_anti_spoof;
        r_vf->strip_stag      = vf->strip_stag;
        r_vf->insert_stag     = vf->insert_stag;
        r_vf->allow_bcast     = vf->allow_bcast;
        r_vf->allow_mcast     = vf->allow_mcast;
        r_vf->allow_un_ucast  = vf->allow_un_ucast;
        
        
        r_vf->last_updated = vf->last_updated;
      }
      
      uint16_t rx_mode = 0;

/*      
      if (vf->allow_bcast)
        rx_mode |= ETH_VMDQ_ACCEPT_BROADCAST;
      if (vf->allow_un_ucast)
        rx_mode |= ETH_VMDQ_ACCEPT_HASH_UC;
      if (vf->allow_mcast)
        rx_mode |= ETH_VMDQ_ACCEPT_MULTICAST;
*/      

      // figure this out if we have to update it every time we change VLANS/MACS 
      // or once when update ports config
      rte_eth_promiscuous_enable(port->rte_port_number);
      rte_eth_allmulticast_enable(port->rte_port_number);  
      ret = rte_eth_dev_uc_all_hash_table_set(port->rte_port_number, on);
      
      
/*
      rx_mode |= ETH_VMDQ_ACCEPT_BROADCAST;
      rx_mode |= ETH_VMDQ_ACCEPT_HASH_UC;
      rx_mode |= ETH_VMDQ_ACCEPT_HASH_MC;
      rx_mode |= ETH_VMDQ_ACCEPT_MULTICAST;
      
      ret = rte_eth_dev_set_vf_rxmode(port->rte_port_number, vf->num, rx_mode,(uint8_t)on);
	
      if (ret < 0)
        traceLog(TRACE_INFO, "rte_eth_dev_set_vf_rxmode(): Bad VF receive mode parameter, return code = %d \n", ret);
*/      
 
      // don't accept untagged frames
      rx_mode = 0;
      rx_mode |= ETH_VMDQ_ACCEPT_UNTAG; 
      ret = rte_eth_dev_set_vf_rxmode(port->rte_port_number, vf->num, rx_mode, !on);
	
      if (ret < 0)
        traceLog(TRACE_DEBUG, "set_vf_allow_untagged(): bad VF receive mode parameter, return code = %d \n", ret);    
  
    }     
  }
  
  running_config.num_ports = sriov_config.num_ports;
  
  return 0;
}



int
readConfigFile(char *fname)
{
 
  //printf("Fname: %s\n", fname);
  
  int num_ports, num_vfs;
  config_t cfg;
  config_setting_t *p_setting, *v_settings, *vl_settings; 

  config_init(&cfg);
 
  if(! config_read_file(&cfg, fname)) {
    fprintf(stderr, "file: %s, %s:%d - %s\n", fname, config_error_file(&cfg),
            config_error_line(&cfg), config_error_text(&cfg));
            
    config_destroy(&cfg);
    exit(EXIT_FAILURE);
  } 

  
  // iterate through all ports
  p_setting = config_lookup(&cfg, "ports");
  if(p_setting != NULL) {
    num_ports = config_setting_length(p_setting);
    
    if(num_ports > MAX_PORTS){
      traceLog(TRACE_ERROR, "too many ports: %d\n", num_ports);
      exit(EXIT_FAILURE);
    }
    
    sriov_config.num_ports = num_ports;
    
    int i;

    for(i = 0; i < num_ports; i++) {
      config_setting_t *port = config_setting_get_elem(p_setting, i);

      // Only output the record if all of the expected fields are present.
      const char *name, *pciid;
      int last_updated, mtu;
    

      if(!(config_setting_lookup_string(port, "name", &name)
           && config_setting_lookup_string(port, "pciid", &pciid)
           && config_setting_lookup_int(port, "last_updated", &last_updated)
           && config_setting_lookup_int(port, "mtu", &mtu)))

        continue;

      
      stpcpy(sriov_config.ports[i].name, name);
      stpcpy(sriov_config.ports[i].pciid, pciid);
      sriov_config.ports[i].last_updated = last_updated;
      sriov_config.ports[i].mtu = mtu;
      

      // not sure how to use this stuff ;(
      char sstr[15];
      sprintf(sstr, "ports.[%d].VFs", i);
      v_settings = config_lookup(&cfg, sstr);
      
      if(v_settings != NULL) {

        num_vfs = config_setting_length(v_settings);
        
        sriov_config.ports[i].num_vfs = num_vfs;
        
        if (num_vfs > MAX_VFS) {
          traceLog(TRACE_ERROR, "too many VF's: %d\n", num_vfs);
          exit(EXIT_FAILURE);
        }
        
        int y;

        
        for(y = 0; y < num_vfs; y++) {
          config_setting_t *vf = config_setting_get_elem(v_settings, y);
            
          /* Only output the record if all of the expected fields are present. */
          int vfn, strip_stag, insert_stag, vlan_anti_spoof, mac_anti_spoof;
          int allow_bcast, allow_mcast, allow_un_ucast, last_updated, link;
          double rate;
       
                
          if(!(config_setting_lookup_int(vf, "vf", &vfn)
               && config_setting_lookup_int(vf, "last_updated", &last_updated)
               && config_setting_lookup_int(vf, "strip_stag", &strip_stag)
               && config_setting_lookup_int(vf, "insert_stag", &insert_stag)
               && config_setting_lookup_int(vf, "vlan_anti_spoof", &vlan_anti_spoof)
               && config_setting_lookup_int(vf, "mac_anti_spoof", &mac_anti_spoof)
               && config_setting_lookup_float(vf, "rate", &rate)
               && config_setting_lookup_int(vf, "link", &link)
               && config_setting_lookup_int(vf, "allow_bcast", &allow_bcast)
               && config_setting_lookup_int(vf, "allow_mcast", &allow_mcast)
               && config_setting_lookup_int(vf, "allow_un_ucast", &allow_un_ucast)))


               
             continue;

          sriov_config.ports[i].vfs[y].num = vfn;
          sriov_config.ports[i].vfs[y].last_updated = last_updated;
          sriov_config.ports[i].vfs[y].strip_stag = strip_stag;
          sriov_config.ports[i].vfs[y].insert_stag = insert_stag;
          sriov_config.ports[i].vfs[y].vlan_anti_spoof = vlan_anti_spoof;
          sriov_config.ports[i].vfs[y].mac_anti_spoof = mac_anti_spoof;
          sriov_config.ports[i].vfs[y].allow_bcast = allow_bcast;
          sriov_config.ports[i].vfs[y].allow_mcast = allow_mcast;
          sriov_config.ports[i].vfs[y].allow_un_ucast = allow_un_ucast;
          sriov_config.ports[i].vfs[y].link = link;
          
          sriov_config.ports[i].vfs[y].rate = rate;
   
          /*
          printf("%-5d  %-36s  %-10d  %-11d  %-8d  %-7d  %-11d  %-11d  %-14d  %2.2f\n", 
                  sriov_config.ports[i].vfs[y].num,
                  sriov_config.ports[i].vfs[y].last_updated,
                  sriov_config.ports[i].vfs[y].strip_stag,
                  sriov_config.ports[i].vfs[y].insert_stag,
                  sriov_config.ports[i].vfs[y].vlan_anti_spoof,
                  sriov_config.ports[i].vfs[y].mac_anti_spoof,
                  sriov_config.ports[i].vfs[y].allow_bcast,
                  sriov_config.ports[i].vfs[y].allow_mcast,
                  sriov_config.ports[i].vfs[y].allow_un_ucast,
                  sriov_config.ports[i].vfs[y].rate = rate);
          */        
 
                  
          char vstr[30];
          sprintf(vstr, "ports.[%d].VFs.[%d].VLANs", i, y); 
          vl_settings = config_lookup(&cfg, vstr);
          
          if(vl_settings != NULL) {
            
            int count = config_setting_length(vl_settings);   
            
            if(count > MAX_VF_VLANS) {
              traceLog(TRACE_ERROR, "too many VLANs: %d\n", count);
              exit(EXIT_FAILURE);
            }
            
            sriov_config.ports[i].vfs[y].num_vlans = count;

            int x;
           // printf("%-5s\n", "VLAN ID");
           
            for(x = 0; x < count; x++) {
              int vlan_id = config_setting_get_int_elem(vl_settings, x);             
              sriov_config.ports[i].vfs[y].vlans[x] = vlan_id;

              //printf("%-5d\n", sriov_config.ports[i].vfs[y].vlans[x]);
            }
          }

          
          sprintf(vstr, "ports.[%d].VFs.[%d].MACs", i, y); 
          vl_settings = config_lookup(&cfg, vstr);
          
          if(vl_settings != NULL) {
            
            int count = config_setting_length(vl_settings);
            int x;
            //printf("%-5s\n", "MAC");

            if(count > MAX_VF_MACS) {
              traceLog(TRACE_ERROR, "too many MACs: %d\n", count);
              exit(EXIT_FAILURE);
            }

            sriov_config.ports[i].vfs[y].num_macs = count;
            
            for(x = 0; x < count; x++) {
              const char *mac = config_setting_get_string_elem(vl_settings, x);           
              strcpy(sriov_config.ports[i].vfs[y].macs[x], mac); 
              //printf("%-5s\n", sriov_config.ports[i].vfs[y].macs[x]);
            }
          }
        }
      }      
    }
  }

  if (debug)
    dump_sriov_config(sriov_config);
  
  return 0;
}



void 
dump_sriov_config(struct sriov_conf_c sriov_config)
{
  traceLog(TRACE_DEBUG, "Number of ports: %d\n", sriov_config.num_ports);
  int i;
  
  for (i = 0; i < sriov_config.num_ports; i++){
    traceLog(TRACE_DEBUG, "Port #: %d, name: %s, pciid %s, last_updated %d, mtu: %d, num_mirrors: %d, num_vfs: %d\n",
          i, sriov_config.ports[i].name, 
          sriov_config.ports[i].pciid, 
          sriov_config.ports[i].last_updated,
          sriov_config.ports[i].mtu,
          sriov_config.ports[i].num_mirros,
          sriov_config.ports[i].num_vfs );
    
    int y;
    for (y = 0; y < sriov_config.ports[i].num_vfs; y++){
      traceLog(TRACE_DEBUG, "VF num: %d, last_updated: %d\nstrip_stag %d\ninsert_stag %d\nvlan_aspoof: %d\nmac_aspoof: %d\nallow_bcast: %d\n\
allow_ucast: %d\nallow_mcast: %d\nallow_untagged: %d\nrate: %f\nlink: %d\num_vlans: %d\nnum_macs: %d\n", 
            sriov_config.ports[i].vfs[y].num, 
            sriov_config.ports[i].vfs[y].last_updated, 
            sriov_config.ports[i].vfs[y].strip_stag,
            sriov_config.ports[i].vfs[y].insert_stag,
            sriov_config.ports[i].vfs[y].vlan_anti_spoof,
            sriov_config.ports[i].vfs[y].mac_anti_spoof,
            sriov_config.ports[i].vfs[y].allow_bcast,
            sriov_config.ports[i].vfs[y].allow_un_ucast,
            sriov_config.ports[i].vfs[y].allow_mcast,
            sriov_config.ports[i].vfs[y].allow_untagged,
            sriov_config.ports[i].vfs[y].rate,
            sriov_config.ports[i].vfs[y].link,
            sriov_config.ports[i].vfs[y].num_vlans,
            sriov_config.ports[i].vfs[y].num_macs);  

      int x;
      traceLog(TRACE_DEBUG, "VLANs [ ");
      for (x = 0; x < sriov_config.ports[i].vfs[y].num_vlans; x++) {
        traceLog(TRACE_DEBUG, "%d ", sriov_config.ports[i].vfs[y].vlans[x]);
      }   
      traceLog(TRACE_DEBUG, "]\n");
      
      int z;
      traceLog(TRACE_DEBUG, "MACs [ ");
      for (z = 0; z < sriov_config.ports[i].vfs[y].num_macs; z++) {
        traceLog(TRACE_DEBUG, "%s ", sriov_config.ports[i].vfs[y].macs[z]);
      }   
      traceLog(TRACE_DEBUG, "]\n");
      traceLog(TRACE_DEBUG, "------------------------------------------------------------------------------\n");
    }
  }
}

int 
main(int argc, char **argv)
{
  int  opt;
  opterr = 0;

  const char * main_help =
	"sriovctl\n"
	"Usage:\n"
  "  sriovctl [options] -f <file_name>\n"
	"  Options:\n"
  "\t -c <mask> Processor affinity mask\n"
  "\t -v <num>  Verbose (if num > 3 foreground) num - verbose level\n"
  "\t -s <num>  syslog facility 0-11 (log_kern - log_ftp) 16-23 (local0-local7) see /usr/include/sys/syslog.h\n"
	"\t -h|?  Display this help screen\n";


  
  //int devnum = 0;
 
 	struct rte_mempool *mbuf_pool;
	//unsigned n_ports;
	
  prog_name = strdup(argv[0]);
  useSyslog = 1; 

	int i;

//	for( i = 0; i < argc; i++)
//		printf("ARGV[%d] = %s\n", i, argv[i]);


  fname = NULL;
  
  // Parse command line options
  while ( (opt = getopt(argc, argv, "hv:c:s:f:")) != -1)
  {
    switch (opt)
    {

    case 'c':
      cpu_mask = atoi(optarg);
      break;
      
    case 'v':
      traceLevel = atoi(optarg);

      if(traceLevel > 6) {
       useSyslog = 0;
       debug = 1;
      }
     break;  
      
    case 'f':
      fname = strdup(optarg);
      break; 

    case 's':
      logFacility = (atoi(optarg) << 3);
      break;
    


    case 'h':
    case '?':
      printf("%s\n", main_help);
      exit(EXIT_FAILURE);
      break;
    }
  }


  if(fname == NULL) {
    printf("%s\n", main_help);
    exit(EXIT_FAILURE);
  }
  
  
  int res = readConfigFile(fname);
  
  if (res < 0)
    rte_exit(EXIT_FAILURE, "Can not parse config file %s\n", fname);

    
  argc -= optind;
  argv += optind;
  optind = 0;


  //argc = 11;
	argc = 12;

  
  
  int argc_port = argc + sriov_config.num_ports * 2;
  
	//char **cli_argv = (char**)malloc(argc * sizeof(char*));
  char **cli_argv = (char**)malloc(argc_port * sizeof(char*));

  
  // add # num of ports * 2 to args, so we can do -w pciid stuff
  for(i = 0; i < argc_port; i ++) {
    cli_argv[i] = (char*)malloc(20 * sizeof(char));
  }

  sprintf(cli_argv[0], "sriovctl");
  
  sprintf(cli_argv[1], "-c");
  sprintf(cli_argv[2], "%#02x", cpu_mask);
  sprintf(cli_argv[3], "-n");
  sprintf(cli_argv[4], "4");
  sprintf(cli_argv[5], "–m");
  sprintf(cli_argv[6], "50");
  sprintf(cli_argv[7], "--file-prefix");
  sprintf(cli_argv[8], "%s", "sriovctl");
  sprintf(cli_argv[9], "--log-level");
  sprintf(cli_argv[10], "%d", 8);
  sprintf(cli_argv[11], "%s", "--no-huge");
  
  
  int y = 0;
  for(i = argc; i < argc_port; i+=2) {
    sprintf(cli_argv[i], "-w");
    sprintf(cli_argv[i + 1], "%s", sriov_config.ports[y].pciid);
      
    traceLog(TRACE_INFO, "PCI num: %d, PCIID: %s\n", y, sriov_config.ports[y].pciid);
    y++;
  }
  
  
	if(!debug) daemonize();
    
			
	// init EAL 
	int ret = rte_eal_init(argc_port, cli_argv);

		
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
	
	rte_set_log_type(RTE_LOGTYPE_PMD && RTE_LOGTYPE_PORT, 0);
	
	traceLog(TRACE_INFO, "LOG LEVEL = %d, LOG TYPE = %d\n", rte_get_log_level(), rte_log_cur_msg_logtype());

    
	rte_set_log_level(8);
	

	n_ports = rte_eth_dev_count();


  if(n_ports != sriov_config.num_ports) {
    traceLog(TRACE_ERROR, "ports found (%d) != ports requested (%d)\n", n_ports, sriov_config.num_ports);  
  }

  traceLog(TRACE_NORMAL, "n_ports = %d\n", n_ports);

  

 /*
  const struct rte_memzone *mz;

  mz = rte_memzone_reserve(IF_PORT_INFO, sizeof(struct ifrate_s), rte_socket_id(), 0);
	if (mz == NULL)
		rte_exit(EXIT_FAILURE, "Cannot reserve memory zone for port information\n");
  memset(mz->addr, 0, sizeof(struct ifrate_s));
  

  ifrate_stats = mz->addr; 
  
  printf("%p\t\n", (void *)ifrate_stats);

  */

	// Creates a new mempool in memory to hold the mbufs.
	mbuf_pool = rte_pktmbuf_pool_create("sriovctl", NUM_MBUFS * n_ports,
                      MBUF_CACHE_SIZE,
                      0, 
                      RTE_MBUF_DEFAULT_BUF_SIZE,
                      rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initialize all ports. */
  u_int16_t portid;
	for (portid = 0; portid < n_ports; portid++)
		if (port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n", portid);
   

   
  int port; 
  
  for(port = 0; port < n_ports; ++port){
    
    struct rte_eth_dev_info dev_info;
    rte_eth_dev_info_get(port, &dev_info);
      
    //struct ether_addr addr;
    rte_eth_macaddr_get(port, &addr);
    traceLog(TRACE_INFO, "Port: %u, MAC: %02" PRIx8 ":%02" PRIx8 ":%02" PRIx8
           ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 ", ",
        (unsigned)port,
        addr.addr_bytes[0], addr.addr_bytes[1],
        addr.addr_bytes[2], addr.addr_bytes[3],
        addr.addr_bytes[4], addr.addr_bytes[5]);


    traceLog(TRACE_INFO, "Driver Name: %s, Index %d, Pkts rx: %lu, ", 
            dev_info.driver_name, dev_info.if_index, st.pcount);
    
    traceLog(TRACE_INFO, "PCI: %04X:%02X:%02X.%01X, Max VF's: %d, Numa: %d\n\n", dev_info.pci_dev->addr.domain, 
            dev_info.pci_dev->addr.bus , dev_info.pci_dev->addr.devid , dev_info.pci_dev->addr.function, 
            dev_info.max_vfs, dev_info.pci_dev->numa_node);

            
    /*
     * rte could inumerate ports different then in config file
     * rte_config_portmap array will hold index to config
     */
    int i;    
    for(i = 0; i < sriov_config.num_ports; ++i) {
      char pciid[16];
      sprintf(pciid, "%04X:%02X:%02X.%01X", 
            dev_info.pci_dev->addr.domain, 
            dev_info.pci_dev->addr.bus, 
            dev_info.pci_dev->addr.devid, 
            dev_info.pci_dev->addr.function);
      
      if (strcmp(pciid, sriov_config.ports[i].pciid) == 0) {;
        rte_config_portmap[port] = i;
        // point config port back to rte port
        sriov_config.ports[i].rte_port_number = port;
      }
    }
  }
  

  struct sigaction sa;

  sa.sa_handler = sig_int;
  sigaction(SIGINT, &sa, NULL);

  sa.sa_handler = sig_int;
  sigaction(SIGTERM, &sa, NULL);

  sa.sa_handler = sig_int;
  sigaction(SIGABRT, &sa, NULL);

  sa.sa_handler = sig_hup;
  sigaction(SIGHUP, &sa, NULL);
  
  sa.sa_handler = sig_usr;
  sigaction(SIGUSR1, &sa, NULL);    
  
  gettimeofday(&st.startTime, NULL);

  traceLog(TRACE_NORMAL, "starting sriovctl loop\n");
  

  update_ports_config();

  char buff[1024];
  if(mkfifo(STATS_FILE, 0666) != 0)
    traceLog(TRACE_ERROR, "can't create pipe: %s, %d\n", STATS_FILE, errno);

  int fd;
  /*
	FILE * dump = fopen("/tmp/pci_dump.txt", "w");
	rte_eal_pci_dump(dump);
	fclose (dump);
	*/
	
  while(!terminated)
	{
		usleep(20000);
   
    fd = open(STATS_FILE, O_WRONLY);
    sprintf(buff, "%s %18s %10s %10s %10s %10s %10s %10s %10s %10s %10s\n", "Iface", "Link", "Speed", "Duplex", "RX pkts", "RX bytes", 
      "RX errors", "RX dropped", "TX pkts", "TX bytes", "TX errors");   
    
    __attribute__((__unused__)) int ret;
    ret = write(fd, buff, strlen(buff));
    
    for (i = 0; i < n_ports; ++i)
    {
			struct rte_eth_dev_info dev_info;
			rte_eth_dev_info_get(i, &dev_info);			

			sprintf(buff, "%04X:%02X:%02X.%01X", 
			dev_info.pci_dev->addr.domain, 
			dev_info.pci_dev->addr.bus, 
			dev_info.pci_dev->addr.devid, 
			dev_info.pci_dev->addr.function);
						
      ret = write(fd, buff, strlen(buff));  
      
      nic_stats_display(i, buff);
      ret = write(fd, buff, strlen(buff));       
    }
    
    close(fd);
   // if (debug)
        //nic_stats_display(i);
	}
 
  if(unlink(STATS_FILE) != 0)
    traceLog(TRACE_ERROR, "can't delete pipe: %s\n", STATS_FILE);
  
  gettimeofday(&st.endTime, NULL);
  traceLog(TRACE_NORMAL, "Duration %.f sec\n", timeDelta(&st.endTime, &st.startTime));

  traceLog(TRACE_NORMAL, "sriovctl exit\n");

  return EXIT_SUCCESS;
}