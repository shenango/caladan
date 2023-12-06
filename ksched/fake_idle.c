#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>

static struct kobject *kobj;
static int unloaded;
static int refcnt_for_unload;

static int __cpuidle fake_idle(struct cpuidle_device *dev,
                                 struct cpuidle_driver *drv, int index)
{
  return index;
}

static struct cpuidle_driver fake_idle_driver = {
        .name = "fake_idle",
        .owner = THIS_MODULE,
        .states = {
                {
                        .enter                  = fake_idle,
                        .exit_latency           = 1,
                        .target_residency       = 1,
                        .name                   = "",
                        .desc                   = "",
		},
        },
        .safe_state_index = 0,
        .state_count = 1,
};

static ssize_t unload_store(struct kobject *kobj, struct kobj_attribute *attr,
                                   const char *buf, size_t count) {
  if (unloaded) return -ENODEV;
  if (atomic_read(&THIS_MODULE->refcnt) + 1 != refcnt_for_unload) return -EBUSY;
  unloaded = 1;
  cpuidle_unregister(&fake_idle_driver);
  return count;
}

static ssize_t unload_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
   return sprintf(buf, "%s\n", unloaded ? "unloaded" : "loaded");
}

static struct kobj_attribute unload_attr = __ATTR(unload, 0644, unload_show, unload_store);

static int __init create_sysfs_entry(void)
{
  int err;

  kobj = kobject_create_and_add("fake_idle", NULL);
  if (kobj == NULL)
    return -ENOMEM;
  err = sysfs_create_file(kobj, &unload_attr.attr);
  if (err)
    kobject_put(kobj);

  return err;
}

static int __init fake_idle_init(void)
{
  int err;


  err = create_sysfs_entry();
  if (err)
    return err;

  err = cpuidle_register(&fake_idle_driver, NULL);
  if (err)
    kobject_put(kobj);

  refcnt_for_unload = atomic_read(&THIS_MODULE->refcnt);
  return err;
}

static void __exit fake_idle_exit(void)
{
  kobject_put(kobj);
}

module_init(fake_idle_init);
module_exit(fake_idle_exit);

MODULE_LICENSE("GPL");
