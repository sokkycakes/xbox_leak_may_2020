#if !(defined(GO) && defined(GOM) && defined(GO2) && defined(DATA))
#error wrapper macros missing
#endif
GOM(xbr_init, iFEup)
GO(xbr_name, pFu)
GO(xbr_invoke, vFup)
GO(xbr_control, vFup)
