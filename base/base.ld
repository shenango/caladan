SECTIONS
{
  .perthread (READONLY) : {
    PROVIDE(__perthread_start = .);
    *(.perthread)
    PROVIDE(__perthread_end = .);
  }
}
INSERT AFTER .text
