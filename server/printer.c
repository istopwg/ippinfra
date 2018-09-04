/*
 * Printer object code for sample IPP server implementation.
 *
 * Copyright © 2014-2018 by the IEEE-ISTO Printer Working Group
 * Copyright © 2010-2018 by Apple Inc.
 *
 * Licensed under Apache License v2.0.  See the file "LICENSE" for more
 * information.
 */

#include "ippserver.h"


/*
 * Local functions...
 */

static int		compare_active_jobs(server_job_t *a, server_job_t *b);
static int		compare_completed_jobs(server_job_t *a, server_job_t *b);
static int		compare_devices(server_device_t *a, server_device_t *b);
static int		compare_jobs(server_job_t *a, server_job_t *b);
static ipp_t		*create_media_col(const char *media, const char *source, const char *type, int width, int length, int margins);
static ipp_t		*create_media_size(int width, int length);
#ifdef HAVE_DNSSD
static void DNSSD_API	dnssd_callback(DNSServiceRef sdRef, DNSServiceFlags flags, DNSServiceErrorType errorCode, const char *name, const char *regtype, const char *domain, server_printer_t *printer);
#elif defined(HAVE_AVAHI)
static void		dnssd_callback(AvahiEntryGroup *p, AvahiEntryGroupState state, void *context);
#endif /* HAVE_DNSSD */
#if defined(HAVE_DNSSD) || defined(HAVE_AVAHI)
static void		register_geo(server_printer_t *printer);
#endif /* HAVE_DNSSD || HAVE_AVAHI */
static int		register_printer(server_printer_t *printer, const char *adminurl, const char *regtype);


/*
 * 'serverCopyPrinterStateReasons()' - Copy printer-state-reasons values.
 */

void
serverCopyPrinterStateReasons(
    ipp_t          *ipp,		/* I - Attributes */
    ipp_tag_t      group_tag,		/* I - Group */
    server_printer_t *printer)		/* I - Printer */
{
  server_preason_t	creasons = printer->state_reasons | printer->dev_reasons;
					/* Combined reasons */


  if (creasons == SERVER_PREASON_NONE)
  {
    ippAddString(ipp, group_tag, IPP_CONST_TAG(IPP_TAG_KEYWORD), "printer-state-reasons", NULL, "none");
  }
  else
  {
    int			i,		/* Looping var */
			num_reasons = 0;/* Number of reasons */
    server_preason_t	reason;		/* Current reason */
    const char		*reasons[32];	/* Reason strings */

    for (i = 0, reason = 1; i < (int)(sizeof(server_preasons) / sizeof(server_preasons[0])); i ++, reason <<= 1)
    {
      if (creasons & reason)
	reasons[num_reasons ++] = server_preasons[i];
    }

    ippAddStrings(ipp, group_tag, IPP_CONST_TAG(IPP_TAG_KEYWORD), "printer-state-reasons", num_reasons, NULL, reasons);
  }
}


/*
 * 'serverCreatePrinter()' - Create, register, and listen for connections to a
 *                           printer object.
 */

server_printer_t *			/* O - Printer */
serverCreatePrinter(
    const char     *resource,		/* I - Resource path for URIs */
    const char     *name,		/* I - printer-name */
    server_pinfo_t *pinfo,		/* I - Printer information */
    int            dupe_pinfo)		/* I - Duplicate printer info strings? */
{
  int			i;		/* Looping var */
  server_printer_t	*printer;	/* Printer */
  cups_array_t		*existing;	/* Existing attributes cache */
  char			title[256];	/* Title for attributes */
  server_listener_t	*lis;		/* Current listener */
  cups_array_t		*uris;		/* Array of URIs */
  int			num_uris;	/* Number of URIs */
  int			is_print3d;	/* 3D printer? */
  char			uri[1024],	/* Printer URI */
			*uriptr,	/* Current URI */
			**uriptrs,	/* All URIs */
			icons[1024],	/* printer-icons URI */
			adminurl[1024],	/* printer-more-info URI */
			supplyurl[1024],/* printer-supply-info-uri URI */
			device_id[1024],/* printer-device-id */
			make_model[128],/* printer-make-and-model */
			uuid[128],	/* printer-uuid */
			spooldir[1024];	/* Per-printer spool directory */
  int			num_formats = 0;/* Number of document-format-supported values */
  char			*defformat = NULL,
					/* document-format-default value */
			*formats[100],	/* document-format-supported values */
			*ptr;		/* Pointer into string */
  const char		*prefix;	/* Prefix string */
  ipp_attribute_t	*format_sup = NULL,
					/* document-format-supported */
			*xri_sup,	/* printer-xri-supported */
			*media_col_database,
					/* media-col-database value */
			*media_size_supported;
					/* media-size-supported value */
  ipp_t			*media_col,	/* media-col-default value */
			*xri_col;	/* printer-xri-supported value */
  int			k_supported;	/* Maximum file size supported */
#ifdef HAVE_STATVFS
  struct statvfs	spoolinfo;	/* FS info for spool directory */
  double		spoolsize;	/* FS size */
#elif defined(HAVE_STATFS)
  struct statfs		spoolinfo;	/* FS info for spool directory */
  double		spoolsize;	/* FS size */
#endif /* HAVE_STATVFS */
  ipp_attribute_t	*attr;		/* Attribute */
  const char		*webscheme;	/* HTTP/HTTPS */
  static const int	orients[4] =	/* orientation-requested-supported values */
  {
    IPP_ORIENT_PORTRAIT,
    IPP_ORIENT_LANDSCAPE,
    IPP_ORIENT_REVERSE_LANDSCAPE,
    IPP_ORIENT_REVERSE_PORTRAIT
  };
  static const char * const versions[] =/* ipp-versions-supported values */
  {
    "1.0",
    "1.1",
    "2.0",
    "2.1",
    "2.2"
  };
  static const char * const features[] =/* ipp-features-supported values */
  {
    "document-object",
    "ipp-everywhere",
    "page-overrides",
    "system-service",
    "infrastructure-printer"
  };
  static const char * const features3d[] =/* ipp-features-supported values */
  {
    "document-object",
    "ipp-3d",
    "system-service",
    "infrastructure-printer"
  };
  static const char * const notify_events_supported[] =
  {					/* notify-events-supported values */
    "document-completed",
    "document-config-changed",
    "document-created",
    "document-fetchable",
    "document-state-changed",
    "document-stopped",
    "job-completed",
    "job-config-changed",
    "job-created",
    "job-fetchable",
    "job-progress",
    "job-state-changed",
    "job-stopped",
    "none",
    "printer-config-changed",
    "printer-created",
    "printer-deleted",
    "printer-finishings-changed",
    "printer-media-changed",
    "printer-queue-order-changed",
    "printer-restarted",
    "printer-shutdown",
    "printer-state-changed",
    "printer-stopped",
    "resource-canceled",
    "resource-config-changed",
    "resource-created",
    "resource-installed",
    "resource-changed",
    "system-config-changed",
    "system-state-changed",
    "system-stopped"
  };
  static const int	ops[] =		/* operations-supported values */
  {
    IPP_OP_PRINT_JOB,
    IPP_OP_PRINT_URI,
    IPP_OP_VALIDATE_JOB,
    IPP_OP_CREATE_JOB,
    IPP_OP_SEND_DOCUMENT,
    IPP_OP_SEND_URI,
    IPP_OP_CANCEL_JOB,
    IPP_OP_GET_JOB_ATTRIBUTES,
    IPP_OP_GET_JOBS,
    IPP_OP_GET_PRINTER_ATTRIBUTES,
    IPP_OP_HOLD_JOB,
    IPP_OP_RELEASE_JOB,
    IPP_OP_PAUSE_PRINTER,
    IPP_OP_RESUME_PRINTER,
    IPP_OP_SET_PRINTER_ATTRIBUTES,
    IPP_OP_SET_JOB_ATTRIBUTES,
    IPP_OP_GET_PRINTER_SUPPORTED_VALUES,
    IPP_OP_CREATE_PRINTER_SUBSCRIPTIONS,
    IPP_OP_CREATE_JOB_SUBSCRIPTIONS,
    IPP_OP_GET_SUBSCRIPTION_ATTRIBUTES,
    IPP_OP_GET_SUBSCRIPTIONS,
    IPP_OP_RENEW_SUBSCRIPTION,
    IPP_OP_CANCEL_SUBSCRIPTION,
    IPP_OP_GET_NOTIFICATIONS,
    IPP_OP_ENABLE_PRINTER,
    IPP_OP_DISABLE_PRINTER,
    IPP_OP_PAUSE_PRINTER_AFTER_CURRENT_JOB,
    IPP_OP_HOLD_NEW_JOBS,
    IPP_OP_RELEASE_HELD_NEW_JOBS,
    IPP_OP_RESTART_PRINTER,
    IPP_OP_SHUTDOWN_PRINTER,
    IPP_OP_STARTUP_PRINTER,
    IPP_OP_CANCEL_CURRENT_JOB,
    IPP_OP_CANCEL_DOCUMENT,
    IPP_OP_GET_DOCUMENT_ATTRIBUTES,
    IPP_OP_GET_DOCUMENTS,
    IPP_OP_SET_DOCUMENT_ATTRIBUTES,
    IPP_OP_CANCEL_JOBS,
    IPP_OP_CANCEL_MY_JOBS,
    IPP_OP_CLOSE_JOB,
    IPP_OP_IDENTIFY_PRINTER,
    IPP_OP_VALIDATE_DOCUMENT,
    IPP_OP_ACKNOWLEDGE_DOCUMENT,
    IPP_OP_ACKNOWLEDGE_IDENTIFY_PRINTER,
    IPP_OP_ACKNOWLEDGE_JOB,
    IPP_OP_FETCH_DOCUMENT,
    IPP_OP_FETCH_JOB,
    IPP_OP_GET_OUTPUT_DEVICE_ATTRIBUTES,
    IPP_OP_UPDATE_ACTIVE_JOBS,
    IPP_OP_UPDATE_DOCUMENT_STATUS,
    IPP_OP_UPDATE_JOB_STATUS,
    IPP_OP_UPDATE_OUTPUT_DEVICE_ATTRIBUTES,
    IPP_OP_DEREGISTER_OUTPUT_DEVICE
  };
  static const int	ops3d[] =	/* operations-supported values */
  {
    IPP_OP_VALIDATE_JOB,
    IPP_OP_CREATE_JOB,
    IPP_OP_SEND_DOCUMENT,
    IPP_OP_SEND_URI,
    IPP_OP_CANCEL_JOB,
    IPP_OP_GET_JOB_ATTRIBUTES,
    IPP_OP_GET_JOBS,
    IPP_OP_GET_PRINTER_ATTRIBUTES,
    IPP_OP_HOLD_JOB,
    IPP_OP_RELEASE_JOB,
    IPP_OP_PAUSE_PRINTER,
    IPP_OP_RESUME_PRINTER,
    IPP_OP_SET_PRINTER_ATTRIBUTES,
    IPP_OP_SET_JOB_ATTRIBUTES,
    IPP_OP_GET_PRINTER_SUPPORTED_VALUES,
    IPP_OP_CREATE_PRINTER_SUBSCRIPTIONS,
    IPP_OP_CREATE_JOB_SUBSCRIPTIONS,
    IPP_OP_GET_SUBSCRIPTION_ATTRIBUTES,
    IPP_OP_GET_SUBSCRIPTIONS,
    IPP_OP_RENEW_SUBSCRIPTION,
    IPP_OP_CANCEL_SUBSCRIPTION,
    IPP_OP_GET_NOTIFICATIONS,
    IPP_OP_ENABLE_PRINTER,
    IPP_OP_DISABLE_PRINTER,
    IPP_OP_PAUSE_PRINTER_AFTER_CURRENT_JOB,
    IPP_OP_HOLD_NEW_JOBS,
    IPP_OP_RELEASE_HELD_NEW_JOBS,
    IPP_OP_RESTART_PRINTER,
    IPP_OP_SHUTDOWN_PRINTER,
    IPP_OP_STARTUP_PRINTER,
    IPP_OP_CANCEL_CURRENT_JOB,
    IPP_OP_CANCEL_DOCUMENT,
    IPP_OP_GET_DOCUMENT_ATTRIBUTES,
    IPP_OP_GET_DOCUMENTS,
    IPP_OP_SET_DOCUMENT_ATTRIBUTES,
    IPP_OP_CANCEL_JOBS,
    IPP_OP_CANCEL_MY_JOBS,
    IPP_OP_CLOSE_JOB,
    IPP_OP_IDENTIFY_PRINTER,
    IPP_OP_VALIDATE_DOCUMENT,
    IPP_OP_ACKNOWLEDGE_DOCUMENT,
    IPP_OP_ACKNOWLEDGE_IDENTIFY_PRINTER,
    IPP_OP_ACKNOWLEDGE_JOB,
    IPP_OP_FETCH_DOCUMENT,
    IPP_OP_FETCH_JOB,
    IPP_OP_GET_OUTPUT_DEVICE_ATTRIBUTES,
    IPP_OP_UPDATE_ACTIVE_JOBS,
    IPP_OP_UPDATE_DOCUMENT_STATUS,
    IPP_OP_UPDATE_JOB_STATUS,
    IPP_OP_UPDATE_OUTPUT_DEVICE_ATTRIBUTES,
    IPP_OP_DEREGISTER_OUTPUT_DEVICE
  };
  static const char * const charsets[] =/* charset-supported values */
  {
    "us-ascii",
    "utf-8"
  };
  static const char * const compressions[] =/* compression-supported values */
  {
#ifdef HAVE_LIBZ
    "deflate",
    "gzip",
#endif /* HAVE_LIBZ */
    "none"
  };
  static const char * const identify_actions[] =
  {
    "display",
    "sound"
  };
  static const char * const doc_creation[] =
  {					/* document-creation-attributes-supported values */
    "copies",
    "document-name",
    "media",
    "media-col",
    "orientation-requested",
    "output-bin",
    "page-ranges",
    "print-color-mode",
    "print-quality",
    "sides"
  };
  static const char * const doc_creation3d[] =
  {					/* document-creation-attributes-supported values for 3D printers */
    "copies",
    "document-name",
    "materials-col",
    "platform-temperature",
    "print-accuracy",
    "print-base",
    "print-quality",
    "print-supports"
  };
  static const char * const job_creation[] =
  {					/* job-creation-attributes-supported values */
    "copies",
    "finishings",
    "finishings-col",
    "ipp-attribute-fidelity",
    "job-account-id",
    "job-accounting-user-id",
    "job-name",
    "job-password",
    "job-priority",
    "media",
    "media-col",
    "multiple-document-handling",
    "orientation-requested",
    "output-bin",
    "page-ranges",
    "print-color-mode",
    "print-quality",
    "sides"
  };
  static const char * const job_creation3d[] =
  {					/* job-creation-attributes-supported values for 3D printers */
    "ipp-attribute-fidelity",
    "job-name",
    "job-priority",
    "materials-col",
    "platform-temperature",
    "print-accuracy",
    "print-base",
    "print-quality",
    "print-supports"
  };
  static const char * const job_hold_until_supported[] =
  {					/* job-hold-until-supported values */
    "no-hold",
    "indefinite",
    "day-time",
    "evening",
    "night",
    "second-shift",
    "third-shift",
    "weekend"
  };
  static const int media_col_sizes[][2] =
  {					/* Default media-col sizes */
    { 21590, 27940 },			/* Letter */
    { 21590, 35560 },			/* Legal */
    { 21000, 29700 }			/* A4 */
  };
  static const char * const media_col_supported[] =
  {					/* media-col-supported values */
    "media-bottom-margin",
    "media-left-margin",
    "media-right-margin",
    "media-size",
    "media-size-name",
    "media-source",
    "media-top-margin",
    "media-type"
  };
  static const char * const media_supported[] =
  {					/* Default media-supported values */
    "na_letter_8.5x11in",		/* Letter */
    "na_legal_8.5x14in",		/* Legal */
    "iso_a4_210x297mm"			/* A4 */
  };
  static const int media_xxx_margin_supported[] =
  {					/* Default media-xxx-margin-supported values */
    635
  };
  static const char * const multiple_document_handling[] =
  {					/* multiple-document-handling-supported values */
    "separate-documents-uncollated-copies",
    "separate-documents-collated-copies"
  };
  static const char * const notify_attributes[] =
  {					/* notify-attributes-supported */
    "printer-state-change-time",
    "notify-lease-expiration-time",
    "notify-subscriber-user-name"
  };
  static const char * const overrides[] =
  {					/* overrides-supported */
    "document-numbers",
    "pages"
  };
  static const char * const print_color_mode_supported[] =
  {					/* print-color-mode-supported values */
    "auto",
    "color",
    "monochrome"
  };
  static const int	print_quality_supported[] =
  {					/* print-quality-supported values */
    IPP_QUALITY_DRAFT,
    IPP_QUALITY_NORMAL,
    IPP_QUALITY_HIGH
  };
  static const char * const printer_supply[] =
  {					/* printer-supply values */
    "index=1;class=receptacleThatIsFilled;type=wasteToner;unit=percent;"
        "maxcapacity=100;level=67;colorantname=unknown;",
    "index=2;class=supplyThatIsConsumed;type=toner;unit=percent;"
        "maxcapacity=100;level=100;colorantname=black;",
    "index=3;class=supplyThatIsConsumed;type=toner;unit=percent;"
        "maxcapacity=100;level=25;colorantname=cyan;",
    "index=4;class=supplyThatIsConsumed;type=toner;unit=percent;"
        "maxcapacity=100;level=50;colorantname=magenta;",
    "index=5;class=supplyThatIsConsumed;type=toner;unit=percent;"
        "maxcapacity=100;level=75;colorantname=yellow;"
  };
  static const char * const printer_supply_desc[] =
  {					/* printer-supply-description values */
    "Toner Waste",
    "Black Toner",
    "Cyan Toner",
    "Magenta Toner",
    "Yellow Toner"
  };
  static const int	pwg_raster_document_resolution_supported[] =
  {
    150,
    300
  };
  static const char * const pwg_raster_document_type_supported[] =
  {
    "black_1",
    "cmyk_8",
    "sgray_8",
    "srgb_8",
    "srgb_16"
  };
  static const char * const reference_uri_schemes_supported[] =
  {					/* reference-uri-schemes-supported */
    "ftp",
    "http",
#ifdef HAVE_SSL
    "https",
#endif /* HAVE_SSL */
    "file"
  };
  static const char * const sides_supported[] =
  {					/* sides-supported values */
    "one-sided",
    "two-sided-long-edge",
    "two-sided-short-edge"
  };
  static const char * const urf_supported[] =
  {					/* urf-supported values */
    "CP1",
    "IS1-5-7",
    "MT1-2-3-4-5-6-8-9-10-11-12-13",
    "RS300",
    "SRGB24",
    "V1.4",
    "W8",
    "DM1"
  };
  static const char * const which_jobs[] =
  {					/* which-jobs-supported values */
    "completed",
    "not-completed",
    "aborted",
    "all",
    "canceled",
    "pending",
    "pending-held",
    "processing",
    "processing-stopped"
  };


  serverLog(SERVER_LOGLEVEL_DEBUG, "serverCreatePrinter(resource=\"%s\", name=\"%s\", pinfo=%p)", resource, name, (void *)pinfo);

  is_print3d = !strncmp(resource, "/ipp/print3d/", 13);

 /*
  * Allocate memory for the printer...
  */

  if ((printer = calloc(1, sizeof(server_printer_t))) == NULL)
  {
    perror("ippserver: Unable to allocate memory for printer");
    return (NULL);
  }

  if ((attr = ippFindAttribute(pinfo->attrs, "printer-id", IPP_TAG_INTEGER)) != NULL)
  {
    printer->id = ippGetInteger(attr, 0);
  }
  else
  {
    printer->id = NextPrinterId ++;

    ippAddInteger(pinfo->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "printer-id", printer->id);
  }

  printer->type           = is_print3d ? SERVER_TYPE_PRINT3D : SERVER_TYPE_PRINT;
  printer->resource       = strdup(resource);
  printer->resourcelen    = strlen(resource);
  printer->name           = strdup(name);
  printer->dnssd_name     = strdup(name);
  printer->start_time     = time(NULL);
  printer->config_time    = printer->start_time;
  printer->state          = IPP_PSTATE_STOPPED;
  printer->state_reasons  = SERVER_PREASON_PAUSED;
  printer->state_time     = printer->start_time;
  printer->jobs           = cupsArrayNew3((cups_array_func_t)compare_jobs, NULL, NULL, 0, NULL, (cups_afree_func_t)serverDeleteJob);
  printer->active_jobs    = cupsArrayNew((cups_array_func_t)compare_active_jobs, NULL);
  printer->completed_jobs = cupsArrayNew((cups_array_func_t)compare_completed_jobs, NULL);
  printer->next_job_id    = 1;
  printer->pinfo          = *pinfo;

  if (dupe_pinfo)
  {
    printer->pinfo.icon             = pinfo->icon ? strdup(pinfo->icon) : NULL;
    printer->pinfo.location         = pinfo->location ? strdup(pinfo->location) : NULL;
    printer->pinfo.make             = pinfo->make ? strdup(pinfo->make) : NULL;
    printer->pinfo.model            = pinfo->model ? strdup(pinfo->model) : NULL;
    printer->pinfo.document_formats = pinfo->document_formats ? strdup(pinfo->document_formats) : NULL;
    printer->pinfo.command          = pinfo->command ? strdup(pinfo->command) : NULL;
    printer->pinfo.device_uri       = pinfo->device_uri ? strdup(pinfo->device_uri) : NULL;
    printer->pinfo.output_format    = pinfo->output_format ? strdup(pinfo->output_format) : NULL;
  }

  uris = cupsArrayNew3((cups_array_func_t)strcmp, NULL, NULL, 0, (cups_acopy_func_t)strdup, (cups_afree_func_t)free);
  for (lis = cupsArrayFirst(Listeners); lis; lis = cupsArrayNext(Listeners))
  {
    httpAssembleURI(HTTP_URI_CODING_ALL, uri, sizeof(uri), SERVER_IPP_SCHEME, NULL, lis->host, lis->port, resource);

    if (!cupsArrayFind(uris, uri))
      cupsArrayAdd(uris, uri);
  }

  num_uris = cupsArrayCount(uris);

  uriptrs = calloc((size_t)num_uris, sizeof(char *));
  for (i = 0, uriptr = cupsArrayFirst(uris); uriptr; i ++, uriptr = cupsArrayNext(uris))
    uriptrs[i] = uriptr;

  printer->devices = cupsArrayNew((cups_array_func_t)compare_devices, NULL);

  if (printer->pinfo.ppm == 0)
  {
    printer->pinfo.ppm = ippGetInteger(ippFindAttribute(printer->pinfo.attrs, "pages-per-minute", IPP_TAG_INTEGER), 0);
    serverLog(SERVER_LOGLEVEL_DEBUG, "Using ppm=%d", printer->pinfo.ppm);
  }

  if (printer->pinfo.ppm_color == 0)
  {
    printer->pinfo.ppm_color = ippGetInteger(ippFindAttribute(printer->pinfo.attrs, "pages-per-minute-color", IPP_TAG_INTEGER), 0);
    serverLog(SERVER_LOGLEVEL_DEBUG, "Using ppm_color=%d", printer->pinfo.ppm_color);
  }

  if ((attr = ippFindAttribute(printer->pinfo.attrs, "sides-supported", IPP_TAG_KEYWORD)) != NULL)
  {
    printer->pinfo.duplex = ippContainsString(attr, "two-sided-long-edge");
    serverLog(SERVER_LOGLEVEL_DEBUG, "Using duplex=%d", printer->pinfo.duplex);
  }

  _cupsRWInit(&(printer->rwlock));

 /*
  * Prepare values for the printer attributes...
  */

  lis = cupsArrayFirst(Listeners);

#ifdef HAVE_SSL
  if (Encryption != HTTP_ENCRYPTION_NEVER)
  {
    httpAssembleURI(HTTP_URI_CODING_ALL, uri, sizeof(uri), SERVER_IPPS_SCHEME, NULL, lis->host, lis->port, resource);
    webscheme = SERVER_HTTPS_SCHEME;
  }
  else
#endif /* HAVE_SSL */
  {
    httpAssembleURI(HTTP_URI_CODING_ALL, uri, sizeof(uri), SERVER_IPP_SCHEME, NULL, lis->host, lis->port, resource);
    webscheme = SERVER_HTTP_SCHEME;
  }

  printer->default_uri = strdup(uri);

  httpAssembleURIf(HTTP_URI_CODING_ALL, icons, sizeof(icons), webscheme, NULL, lis->host, lis->port, "%s/icon.png", resource);
  httpAssembleURI(HTTP_URI_CODING_ALL, adminurl, sizeof(adminurl), webscheme, NULL, lis->host, lis->port, resource);
  httpAssembleURIf(HTTP_URI_CODING_ALL, supplyurl, sizeof(supplyurl), webscheme, NULL, lis->host, lis->port, "%s/supplies", resource);

  serverLogPrinter(SERVER_LOGLEVEL_INFO, printer, "printer-uri=\"%s\"", (char *)cupsArrayFirst(uris));
  serverLogPrinter(SERVER_LOGLEVEL_DEBUG, printer, "printer-more-info=\"%s\"", adminurl);
  serverLogPrinter(SERVER_LOGLEVEL_DEBUG, printer, "printer-supply-info-uri=\"%s\"", supplyurl);

  if (printer->pinfo.document_formats)
  {
    num_formats = 1;
    formats[0]  = strdup(printer->pinfo.document_formats);
    defformat   = formats[0];
    for (ptr = strchr(formats[0], ','); ptr; ptr = strchr(ptr, ','))
    {
      *ptr++ = '\0';
      formats[num_formats++] = ptr;

      if (!strcasecmp(ptr, "application/octet-stream"))
	defformat = ptr;
    }
  }

 /*
  * Create the printer's spool directory...
  */

  snprintf(spooldir, sizeof(spooldir), "%s/%s", SpoolDirectory, printer->name);
  if (mkdir(spooldir, 0755) && errno != EEXIST)
    serverLog(SERVER_LOGLEVEL_ERROR, "Unable to create spool directory \"%s\": %s", spooldir, strerror(errno));

 /*
  * Get the maximum spool size based on the size of the filesystem used for
  * the spool directory.  If the host OS doesn't support the statfs call
  * or the filesystem is larger than 2TiB, always report INT_MAX.
  */

#ifdef HAVE_STATVFS
  if (statvfs(spooldir, &spoolinfo))
    k_supported = INT_MAX;
  else if ((spoolsize = (double)spoolinfo.f_frsize *
                        spoolinfo.f_blocks / 1024) > INT_MAX)
    k_supported = INT_MAX;
  else
    k_supported = (int)spoolsize;

#elif defined(HAVE_STATFS)
  if (statfs(spooldir, &spoolinfo))
    k_supported = INT_MAX;
  else if ((spoolsize = (double)spoolinfo.f_bsize *
                        spoolinfo.f_blocks / 1024) > INT_MAX)
    k_supported = INT_MAX;
  else
    k_supported = (int)spoolsize;

#else
  k_supported = INT_MAX;
#endif /* HAVE_STATVFS */

 /*
  * Create the printer attributes.  This list of attributes is sorted to improve
  * performance when the client provides a requested-attributes attribute...
  */

  if (!printer->pinfo.attrs)
    printer->pinfo.attrs = ippNew();

  existing = cupsArrayNew((cups_array_func_t)strcmp, NULL);
  for (attr = ippFirstAttribute(printer->pinfo.attrs); attr; attr = ippNextAttribute(printer->pinfo.attrs))
  {
    const char *attrname = ippGetName(attr);/* Attribute name */

    if (attrname)
      cupsArrayAdd(existing, (void *)attrname);
  }

  /* charset-configured */
  ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_CHARSET), "charset-configured", NULL, "utf-8");

  /* charset-supported */
  ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_CHARSET), "charset-supported", sizeof(charsets) / sizeof(charsets[0]), NULL, charsets);

  /* color-supported */
  if (!is_print3d)
  {
    if (!cupsArrayFind(existing, (void *)"color-supported"))
      ippAddBoolean(printer->pinfo.attrs, IPP_TAG_PRINTER, "color-supported", printer->pinfo.ppm_color > 0);
  }

  /* compression-supported */
  if (!cupsArrayFind(existing, (void *)"compression-supported"))
    ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "compression-supported", (int)(sizeof(compressions) / sizeof(compressions[0])), NULL, compressions);

  /* copies-default */
  if (!cupsArrayFind(existing, (void *)"copies-default"))
    ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "copies-default", 1);

  /* copies-supported */
  if (!cupsArrayFind(existing, (void *)"copies-supported"))
    ippAddRange(printer->pinfo.attrs, IPP_TAG_PRINTER, "copies-supported", 1, is_print3d ? 1 : 999);

  /* document-creation-attributes-supported */
  if (!cupsArrayFind(existing, (void *)"document-creation-attributes-supported"))
  {
    if (is_print3d)
      ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "document-creation-attributes-supported", sizeof(doc_creation3d) / sizeof(doc_creation3d[0]), NULL, doc_creation3d);
    else
      ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "document-creation-attributes-supported", sizeof(doc_creation) / sizeof(doc_creation[0]), NULL, doc_creation);
  }

  /* document-format-default */
  if (defformat && !cupsArrayFind(existing, (void *)"document-format-default"))
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE, "document-format-default", NULL, defformat);

  /* document-format-supported */
  if ((format_sup = ippFindAttribute(printer->pinfo.attrs, "document-format-supported", IPP_TAG_ZERO)) == NULL && num_formats > 0)
    format_sup = ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE, "document-format-supported", num_formats, NULL, (const char * const *)formats);

  /* document-password-supported */
  if (!cupsArrayFind(existing, (void *)"document-password-supported"))
    ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "document-password-supported", 127);

  /* finishings-default */
  if (!is_print3d && !cupsArrayFind(existing, (void *)"finishings-default"))
    ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "finishings-default", IPP_FINISHINGS_NONE);

  /* finishings-supported */
  if (!is_print3d && !cupsArrayFind(existing, (void *)"finishings-supported"))
    ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "finishings-supported", IPP_FINISHINGS_NONE);

  /* generated-natural-language-supported */
  ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_LANGUAGE), "generated-natural-language-supported", NULL, "en");

  /* identify-actions-default */
  if (!cupsArrayFind(existing, (void *)"identify-actions-default"))
    ippAddString (printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "identify-actions-default", NULL, "sound");

  /* identify-actions-supported */
  if (!cupsArrayFind(existing, (void *)"identify-actions-supported"))
    ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "identify-actions-supported", sizeof(identify_actions) / sizeof(identify_actions[0]), NULL, identify_actions);

  /* ipp-features-supported */
  if (!cupsArrayFind(existing, (void *)"ipp-features-supported"))
  {
    if (is_print3d)
      ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "ipp-features-supported", sizeof(features3d) / sizeof(features3d[0]) - (printer->pinfo.proxy_group == SERVER_GROUP_NONE), NULL, features3d);
    else
      ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "ipp-features-supported", sizeof(features) / sizeof(features[0]) - (printer->pinfo.proxy_group == SERVER_GROUP_NONE), NULL, features);
  }

  /* ipp-versions-supported */
  if (!cupsArrayFind(existing, (void *)"ipp-versions-supported"))
    ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "ipp-versions-supported", sizeof(versions) / sizeof(versions[0]), NULL, versions);

  /* ippget-event-life */
  ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "ippget-event-life", SERVER_IPPGET_EVENT_LIFE);

  /* job-account-id-default */
  if (!is_print3d && !cupsArrayFind(existing, (void *)"job-account-id-default"))
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_NAME), "job-account-id-default", NULL, "");

  /* job-account-id-supported */
  if (!is_print3d && !cupsArrayFind(existing, (void *)"job-account-id-supported"))
    ippAddBoolean(printer->pinfo.attrs, IPP_TAG_PRINTER, "job-account-id-supported", 1);

  /* job-accounting-user-id-default */
  if (!is_print3d && !cupsArrayFind(existing, (void *)"job-accounting-user-id-default"))
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_NAME), "job-accounting-user-id-default", NULL, "");

  /* job-accounting-user-id-supported */
  if (!is_print3d && !cupsArrayFind(existing, (void *)"job-accounting-user-id-supported"))
    ippAddBoolean(printer->pinfo.attrs, IPP_TAG_PRINTER, "job-accounting-user-id-supported", 1);

  /* job-creation-attributes-supported */
  if (!cupsArrayFind(existing, (void *)"job-creation-attributes-supported"))
  {
    if (is_print3d)
      ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "job-creation-attributes-supported", sizeof(job_creation3d) / sizeof(job_creation3d[0]), NULL, job_creation3d);
    else
      ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "job-creation-attributes-supported", sizeof(job_creation) / sizeof(job_creation[0]), NULL, job_creation);
  }

  /* job-hold-until-supported */
  ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "job-hold-until-supported", sizeof(job_hold_until_supported) / sizeof(job_hold_until_supported[0]), NULL, job_hold_until_supported);

  /* job-hold-until-time-supported */
  ippAddRange(printer->pinfo.attrs, IPP_TAG_PRINTER, "job-hold-until-time-supported", 0, INT_MAX);

  /* job-ids-supported */
  ippAddBoolean(printer->pinfo.attrs, IPP_TAG_PRINTER, "job-ids-supported", 1);

  /* job-k-octets-supported */
  ippAddRange(printer->pinfo.attrs, IPP_TAG_PRINTER, "job-k-octets-supported", 0,
	      k_supported);

  /* job-password-encryption-supported */
  if (!is_print3d && !cupsArrayFind(existing, (void *)"job-password-encryption-supported"))
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "job-password-encryption-supported", NULL, "none");

  /* job-password-supported */
  if (!is_print3d && !cupsArrayFind(existing, (void *)"job-password-supported"))
    ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "job-password-supported", 4);

  /* job-priority-default */
  if (!cupsArrayFind(existing, (void *)"job-priority-default"))
    ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "job-priority-default", 50);

  /* job-priority-supported */
  if (!cupsArrayFind(existing, (void *)"job-priority-supported"))
    ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "job-priority-supported", 100);

  if (!is_print3d)
  {
    /* media-bottom-margin-supported */
    if (!cupsArrayFind(existing, (void *)"media-bottom-margin-supported"))
      ippAddIntegers(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-bottom-margin-supported", (int)(sizeof(media_xxx_margin_supported) / sizeof(media_xxx_margin_supported[0])), media_xxx_margin_supported);

    /* media-col-database */
    if (!cupsArrayFind(existing, (void *)"media-col-database"))
    {
      media_col_database = ippAddCollections(printer->pinfo.attrs, IPP_TAG_PRINTER, "media-col-database", (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0])), NULL);
      for (i = 0; i < (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0])); i ++)
      {
	media_col = create_media_col(media_supported[i], "main", "auto", media_col_sizes[i][0], media_col_sizes[i][1], media_xxx_margin_supported[0]);

	ippSetCollection(printer->pinfo.attrs, &media_col_database, i, media_col);

	ippDelete(media_col);
      }
    }

    /* media-col-default */
    if (!cupsArrayFind(existing, (void *)"media-col-default"))
    {
      media_col = create_media_col(media_supported[0], "main", "auto", media_col_sizes[0][0], media_col_sizes[0][1], media_xxx_margin_supported[0]);

      ippAddCollection(printer->pinfo.attrs, IPP_TAG_PRINTER, "media-col-default", media_col);
      ippDelete(media_col);
    }

    /* media-col-ready */
    if (!cupsArrayFind(existing, (void *)"media-col-ready"))
    {
      media_col = create_media_col(media_supported[0], "main", "auto", media_col_sizes[0][0], media_col_sizes[0][1], media_xxx_margin_supported[0]);

      ippAddCollection(printer->pinfo.attrs, IPP_TAG_PRINTER, "media-col-ready", media_col);
      ippDelete(media_col);
    }

    /* media-default */
    if (!cupsArrayFind(existing, (void *)"media-default"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "media-default", NULL, media_supported[0]);

    /* media-left-margin-supported */
    if (!cupsArrayFind(existing, (void *)"media-left-margin-supported"))
      ippAddIntegers(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-left-margin-supported", (int)(sizeof(media_xxx_margin_supported) / sizeof(media_xxx_margin_supported[0])), media_xxx_margin_supported);

    /* media-ready */
    if (!cupsArrayFind(existing, (void *)"media-ready"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "media-ready", NULL, media_supported[0]);

    /* media-right-margin-supported */
    if (!cupsArrayFind(existing, (void *)"media-right-margin-supported"))
      ippAddIntegers(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-right-margin-supported", (int)(sizeof(media_xxx_margin_supported) / sizeof(media_xxx_margin_supported[0])), media_xxx_margin_supported);

    /* media-supported */
    if (!cupsArrayFind(existing, (void *)"media-supported"))
      ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "media-supported", (int)(sizeof(media_supported) / sizeof(media_supported[0])), NULL, media_supported);

    /* media-size-supported */
    if (!cupsArrayFind(existing, (void *)"media-size-supported"))
    {
      media_size_supported = ippAddCollections(printer->pinfo.attrs, IPP_TAG_PRINTER, "media-size-supported", (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0])), NULL);

      for (i = 0;
	   i < (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0]));
	   i ++)
      {
	ipp_t *size = create_media_size(media_col_sizes[i][0], media_col_sizes[i][1]);

	ippSetCollection(printer->pinfo.attrs, &media_size_supported, i, size);
	ippDelete(size);
      }
    }

    /* media-source-supported */
    if (!cupsArrayFind(existing, (void *)"media-source-supported"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "media-source-supported", NULL, "main");

    /* media-top-margin-supported */
    if (!cupsArrayFind(existing, (void *)"media-top-margin-supported"))
      ippAddIntegers(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-top-margin-supported", (int)(sizeof(media_xxx_margin_supported) / sizeof(media_xxx_margin_supported[0])), media_xxx_margin_supported);

    /* media-type-supported */
    if (!cupsArrayFind(existing, (void *)"media-type-supported"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "media-type-supported", NULL, "auto");

    /* media-col-supported */
    if (!cupsArrayFind(existing, (void *)"media-col-supported"))
      ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "media-col-supported", (int)(sizeof(media_col_supported) / sizeof(media_col_supported[0])), NULL, media_col_supported);

    /* multiple-document-handling-supported */
    ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "multiple-document-handling-supported", sizeof(multiple_document_handling) / sizeof(multiple_document_handling[0]), NULL, multiple_document_handling);
  }

  /* multiple-document-jobs-supported */
  ippAddBoolean(printer->pinfo.attrs, IPP_TAG_PRINTER, "multiple-document-jobs-supported", 0);

  /* multiple-operation-time-out */
  ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "multiple-operation-time-out", 60);

  /* multiple-operation-time-out-action */
  ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "multiple-operation-time-out-action", NULL, "abort-job");

  /* natural-language-configured */
  ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER,
               IPP_CONST_TAG(IPP_TAG_LANGUAGE),
               "natural-language-configured", NULL, "en");

  /* notify-attributes-supported */
  ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "notify-attributes-supported", sizeof(notify_attributes) / sizeof(notify_attributes[0]), NULL, notify_attributes);

  /* notify-events-default */
  ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "notify-events-default", NULL, "job-completed");

  /* notify-events-supported */
  ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "notify-events-supported", sizeof(notify_events_supported) / sizeof(notify_events_supported[0]), NULL, notify_events_supported);

  /* notify-lease-duration-default */
  ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "notify-lease-duration-default", SERVER_NOTIFY_LEASE_DURATION_DEFAULT);

  /* notify-lease-duration-supported */
  ippAddRange(printer->pinfo.attrs, IPP_TAG_PRINTER, "notify-lease-duration-supported", 0, SERVER_NOTIFY_LEASE_DURATION_MAX);

  /* notify-max-events-supported */
  ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "notify-max-events-supported", (int)(sizeof(server_events) / sizeof(server_events[0])));

  /* notify-pull-method-supported */
  ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "notify-pull-method-supported", NULL, "ippget");

  /* number-up-default */
  if (!is_print3d && !cupsArrayFind(existing, (void *)"number-up-default"))
    ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "number-up-default", 1);

  /* number-up-supported */
  if (!is_print3d && !cupsArrayFind(existing, (void *)"number-up-supported"))
    ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "number-up-supported", 1);

  /* operations-supported */
  if (is_print3d)
    ippAddIntegers(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "operations-supported", sizeof(ops3d) / sizeof(ops3d[0]), ops3d);
  else
    ippAddIntegers(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "operations-supported", sizeof(ops) / sizeof(ops[0]), ops);

  if (!is_print3d)
  {
    /* orientation-requested-default */
    if (!cupsArrayFind(existing, (void *)"orientation-requested-default"))
      ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_NOVALUE, "orientation-requested-default", 0);

    /* orientation-requested-supported */
    if (!cupsArrayFind(existing, (void *)"orientation-requested-supported"))
      ippAddIntegers(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "orientation-requested-supported", 4, orients);

    /* output-bin-default */
    if (!cupsArrayFind(existing, (void *)"output-bin-default"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "output-bin-default", NULL, "face-down");

    /* output-bin-supported */
    if (!cupsArrayFind(existing, (void *)"output-bin-supported"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "output-bin-supported", NULL, "face-down");

    /* overrides-supported */
    if (!cupsArrayFind(existing, (void *)"overrides-supported"))
      ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "overrides-supported", (int)(sizeof(overrides) / sizeof(overrides[0])), NULL, overrides);

    /* page-ranges-supported */
    if (!cupsArrayFind(existing, (void *)"page-ranges-supported"))
      ippAddBoolean(printer->pinfo.attrs, IPP_TAG_PRINTER, "page-ranges-supported", 1);

    /* pages-per-minute */
    if (!cupsArrayFind(existing, (void *)"pages-per-minute"))
      ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "pages-per-minute", printer->pinfo.ppm);

    /* pages-per-minute-color */
    if (printer->pinfo.ppm_color > 0 && !cupsArrayFind(existing, (void *)"pages-per-minute-color"))
      ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "pages-per-minute-color", printer->pinfo.ppm_color);

    /* pdl-override-supported */
    if (!cupsArrayFind(existing, (void *)"pdl-override-supported"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "pdl-override-supported", NULL, "attempted");

    /* preferred-attributes-supported */
    ippAddBoolean(printer->pinfo.attrs, IPP_TAG_PRINTER, "preferred-attributes-supported", 0);

    /* print-color-mode-default */
    if (!cupsArrayFind(existing, (void *)"print-color-mode-default"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "print-color-mode-default", NULL, "auto");

    /* print-color-mode-supported */
    if (!cupsArrayFind(existing, (void *)"print-color-mode-supported"))
      ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "print-color-mode-supported", (int)(sizeof(print_color_mode_supported) / sizeof(print_color_mode_supported[0])), NULL, print_color_mode_supported);

    /* print-content-optimize-default */
    if (!cupsArrayFind(existing, (void *)"print-content-optimize-default"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "print-content-optimize-default", NULL, "auto");

    /* print-content-optimize-supported */
    if (!cupsArrayFind(existing, (void *)"print-content-optimize-supported"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "print-content-optimize-supported", NULL, "auto");

    /* print-rendering-intent-default */
    if (!cupsArrayFind(existing, (void *)"print-rendering-intent-default"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "print-rendering-intent-default", NULL, "auto");

    /* print-rendering-intent-supported */
    if (!cupsArrayFind(existing, (void *)"print-rendering-intent-supported"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "print-rendering-intent-supported", NULL, "auto");
  }

  /* print-quality-default */
  if (!cupsArrayFind(existing, (void *)"print-quality-default"))
    ippAddInteger(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "print-quality-default", IPP_QUALITY_NORMAL);

  /* print-quality-supported */
  if (!cupsArrayFind(existing, (void *)"print-quality-supported"))
    ippAddIntegers(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "print-quality-supported", (int)(sizeof(print_quality_supported) / sizeof(print_quality_supported[0])), print_quality_supported);

  /* printer-device-id */
  if (!is_print3d)
  {
    int count = ippGetCount(format_sup);/* Number of supported formats */

    snprintf(device_id, sizeof(device_id), "MFG:%s;MDL:%s;", printer->pinfo.make, printer->pinfo.model);
    ptr    = device_id + strlen(device_id);
    prefix = "CMD:";
    for (i = 0; i < count; i ++)
    {
      const char *format = ippGetString(format_sup, i, NULL);
					/* Current format */

      if (!strcasecmp(format, "application/pdf"))
	snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%sPDF", prefix);
      else if (!strcasecmp(format, "application/postscript"))
	snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%sPS", prefix);
      else if (!strcasecmp(format, "application/vnd.hp-PCL"))
	snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%sPCL", prefix);
      else if (!strcasecmp(format, "image/jpeg"))
	snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%sJPEG", prefix);
      else if (!strcasecmp(format, "image/png"))
	snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%sPNG", prefix);
      else if (!strcasecmp(format, "image/urf"))
	snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%sURF", prefix);
      else if (strcasecmp(format, "application/octet-stream"))
	snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%s%s", prefix, format);

      ptr += strlen(ptr);
      prefix = ",";
    }
    if (ptr < (device_id + sizeof(device_id) - 1))
    {
      *ptr++ = ';';
      *ptr = '\0';
    }

    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-device-id", NULL, device_id);
  }

  /* printer-get-attributes-supported */
  ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "printer-get-attributes-supported", NULL, "document-format");

  /* printer-geo-location */
  if (!cupsArrayFind(existing, (void *)"printer-geo-location"))
    ippAddOutOfBand(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_UNKNOWN, "printer-geo-location");

  /* printer-icons */
  ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-icons", NULL, icons);

  /* printer-info */
  ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-info", NULL, name);

  if (!is_print3d)
  {
    /* printer-input-tray */
    if (!cupsArrayFind(existing, (void *)"printer-input-tray"))
    {
      const char *tray = "type=sheetFeedAutoRemovableTray;mediafeed=0;mediaxfeed=0;maxcapacity=250;level=100;status=0;name=main;";

      ippAddOctetString(printer->pinfo.attrs, IPP_TAG_PRINTER, "printer-input-tray", tray, (int)strlen(tray));
    }
  }

  /* printer-location */
  if (!cupsArrayFind(existing, (void *)"printer-location"))
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-location", NULL, printer->pinfo.location);

  /* printer-make-and-model */
  if (!cupsArrayFind(existing, (void *)"printer-make-and-model"))
  {
    snprintf(make_model, sizeof(make_model), "%s %s", printer->pinfo.make, printer->pinfo.model);

    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-make-and-model", NULL, make_model);
  }

  /* printer-mandatory-job-attributes */
  if (printer->pinfo.pin && !cupsArrayFind(existing, (void *)"printer-mandatory-job-attributes"))
  {
    static const char * const names[] =	/* Attributes needed for PIN printing */
    {
      "job-account-id",
      "job-accounting-user-id",
      "job-password"
    };

    ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "printer-mandatory-job-attributes", (int)(sizeof(names) / sizeof(names[0])), NULL, names);
  }

  /* printer-more-info */
  ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-more-info", NULL, adminurl);

  /* printer-name */
  if (!cupsArrayFind(existing, (void *)"printer-name"))
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", NULL, name);

  /* printer-organization */
  if (!cupsArrayFind(existing, (void *)"printer-organization"))
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_TEXT), "printer-organization", NULL, "Apple Inc.");

  /* printer-organizational-unit */
  if (!cupsArrayFind(existing, (void *)"printer-organizational-unit"))
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_TEXT), "printer-organizational-unit", NULL, "Printing Engineering");

  if (!is_print3d)
  {
    /* printer-resolution-default */
    if (!cupsArrayFind(existing, (void *)"printer-resolution-default"))
      ippAddResolution(printer->pinfo.attrs, IPP_TAG_PRINTER, "printer-resolution-default", IPP_RES_PER_INCH, 600, 600);

    /* printer-resolution-supported */
    if (!cupsArrayFind(existing, (void *)"printer-resolutions-supported"))
      ippAddResolution(printer->pinfo.attrs, IPP_TAG_PRINTER, "printer-resolution-supported", IPP_RES_PER_INCH, 600, 600);
  }

  /* printer-strings-languages-supported */
  if (!cupsArrayFind(existing, (void *)"printer-strings-languages-supported") && printer->pinfo.strings)
  {
    server_lang_t *lang;

    for (attr = NULL, lang = (server_lang_t *)cupsArrayFirst(printer->pinfo.strings); lang; lang = (server_lang_t *)cupsArrayNext(printer->pinfo.strings))
    {
      if (attr)
        ippSetString(printer->pinfo.attrs, &attr, ippGetCount(attr), lang->lang);
      else
        attr = ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_LANGUAGE, "printer-strings-languages-supported", NULL, lang->lang);
    }
  }

  if (!is_print3d)
  {
    /* printer-supply */
    if (!cupsArrayFind(existing, (void *)"printer-supply"))
    {
      int count = printer->pinfo.ppm_color > 0 ? 5 : 2;
					/* Number of values */

      attr = ippAddOctetString(printer->pinfo.attrs, IPP_TAG_PRINTER, "printer-supply", printer_supply[0], (int)strlen(printer_supply[0]));
      for (i = 1; i < count; i ++)
        ippSetOctetString(printer->pinfo.attrs, &attr, i, printer_supply[i], (int)strlen(printer_supply[i]));
    }

    /* printer-supply-description */
    if (!cupsArrayFind(existing, (void *)"printer-supply-description"))
      ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_TEXT), "printer-supply-description", printer->pinfo.ppm_color > 0 ? 5 : 2, NULL, printer_supply_desc);

    /* printer-supply-info-uri */
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-supply-info-uri", NULL, supplyurl);
  }

  /* printer-uri-supported */
  ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-uri-supported", num_uris, NULL, (const char **)uriptrs);

  /* printer-uuid */
  if (!cupsArrayFind(existing, (void *)"printer-uuid"))
  {
    httpAssembleUUID(lis->host, lis->port, name, 0, uuid, sizeof(uuid));
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-uuid", NULL, uuid);
  }

  /* printer-xri-supported */
  xri_sup = ippAddCollections(printer->pinfo.attrs, IPP_TAG_PRINTER, "printer-xri-supported", num_uris, NULL);
  for (i = 0; i < num_uris; i ++)
  {
    xri_col = ippNew();

    ippAddString(xri_col, IPP_TAG_ZERO, IPP_CONST_TAG(IPP_TAG_KEYWORD), "xri-authentication", NULL, Authentication ? "basic"  : "none");

#ifdef HAVE_SSL
    if (Encryption != HTTP_ENCRYPTION_NEVER)
      ippAddString(xri_col, IPP_TAG_ZERO, IPP_CONST_TAG(IPP_TAG_KEYWORD), "xri-security", NULL, "tls");
    else
#endif /* HAVE_SSL */
      ippAddString(xri_col, IPP_TAG_ZERO, IPP_CONST_TAG(IPP_TAG_KEYWORD), "xri-security", NULL, "none");

    ippAddString(xri_col, IPP_TAG_ZERO, IPP_TAG_URI, "xri-uri", NULL, uriptrs[i]);

    ippSetCollection(printer->pinfo.attrs, &xri_sup, i, xri_col);
    ippDelete(xri_col);
  }

  cupsArrayDelete(uris);
  free(uriptrs);

  /* pwg-raster-document-xxx-supported */
  for (i = 0; i < num_formats; i ++)
    if (!strcasecmp(formats[i], "image/pwg-raster"))
      break;

  if (i < num_formats)
  {
    if (!cupsArrayFind(existing, (void *)"pwg-raster-document-resolution-supported"))
      ippAddResolutions(printer->pinfo.attrs, IPP_TAG_PRINTER, "pwg-raster-document-resolution-supported", (int)(sizeof(pwg_raster_document_resolution_supported) / sizeof(pwg_raster_document_resolution_supported[0])), IPP_RES_PER_INCH, pwg_raster_document_resolution_supported, pwg_raster_document_resolution_supported);
    if (!cupsArrayFind(existing, (void *)"pwg-raster-document-sheet-back"))
      ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", NULL, "normal");
    if (!cupsArrayFind(existing, (void *)"pwg-raster-document-type-supported"))
      ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-type-supported", (int)(sizeof(pwg_raster_document_type_supported) / sizeof(pwg_raster_document_type_supported[0])), NULL, pwg_raster_document_type_supported);
  }

  /* reference-uri-scheme-supported */
  ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_URISCHEME), "reference-uri-schemes-supported", (int)(sizeof(reference_uri_schemes_supported) / sizeof(reference_uri_schemes_supported[0])) - !FileDirectories, NULL, reference_uri_schemes_supported);

  /* sides-default */
  if (!is_print3d && !cupsArrayFind(existing, (void *)"sides-default"))
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "sides-default", NULL, "one-sided");

  /* sides-supported */
  if (!is_print3d && !cupsArrayFind(existing, (void *)"sides-supported"))
    ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "sides-supported", printer->pinfo.duplex ? 3 : 1, NULL, sides_supported);

  /* urf-supported */
  for (i = 0; i < num_formats; i ++)
    if (!strcasecmp(formats[i], "image/urf"))
      break;

  if (i < num_formats && !cupsArrayFind(existing, "urf-supported"))
    ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "urf-supported", (int)(sizeof(urf_supported) / sizeof(urf_supported[0])) - !printer->pinfo.duplex, NULL, urf_supported);

  /* uri-authentication-supported */
  attr = ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "uri-authentication-supported", num_uris, NULL, NULL);
  for (i = 0; i < num_uris; i ++)
    ippSetString(printer->pinfo.attrs, &attr, i, Authentication ? "basic"  : "none");

  /* uri-security-supported */
#ifdef HAVE_SSL
  if (Encryption != HTTP_ENCRYPTION_NEVER)
  {
    attr = ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "uri-security-supported", num_uris, NULL, NULL);
    for (i = 0; i < num_uris; i ++)
      ippSetString(printer->pinfo.attrs, &attr, i, "tls");
  }
  else
#endif /* HAVE_SSL */
  {
    attr = ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "uri-security-supported", num_uris, NULL, NULL);
    for (i = 0; i < num_uris; i ++)
      ippSetString(printer->pinfo.attrs, &attr, i, "none");
  }

  /* which-jobs-supported */
  ippAddStrings(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "which-jobs-supported", sizeof(which_jobs) / sizeof(which_jobs[0]), NULL, which_jobs);

  /* xri-authentication-supported */
  ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "xri-authentication-supported", NULL, Authentication ? "basic" : "none");

  /* xri-security-supported */
#ifdef HAVE_SSL
  if (Encryption != HTTP_ENCRYPTION_NEVER)
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "xri-security-supported", NULL, "tls");
  else
#endif /* HAVE_SSL */
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "xri-security-supported", NULL, "none");

  /* xri-uri-scheme-supported */
#ifdef HAVE_SSL
  if (Encryption != HTTP_ENCRYPTION_NEVER)
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_URISCHEME), "xri-uri-scheme-supported", NULL, "ipps");
  else
#endif /* HAVE_SSL */
    ippAddString(printer->pinfo.attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_URISCHEME), "xri-uri-scheme-supported", NULL, "ipp");

  if (num_formats > 0)
    free(formats[0]);

  cupsArrayDelete(existing);

  snprintf(title, sizeof(title), "[Printer %s]", printer->name);
  serverLogAttributes(NULL, title, printer->pinfo.attrs, 0);

 /*
  * Register the printer with Bonjour...
  */

  if (!register_printer(printer, adminurl, DNSSDSubType))
    goto bad_printer;

 /*
  * Return it!
  */

  return (printer);


 /*
  * If we get here we were unable to create the printer...
  */

  bad_printer:

  serverDeletePrinter(printer);
  return (NULL);
}


/*
 * 'serverDeletePrinter()' - Unregister, close listen sockets, and free all memory
 *                      used by a printer object.
 */

void
serverDeletePrinter(server_printer_t *printer)	/* I - Printer */
{
  _cupsRWLockWrite(&printer->rwlock);

#if HAVE_DNSSD
  if (printer->geo_ref)
    DNSServiceRemoveRecord(printer->printer_ref, printer->geo_ref, 0);
  if (printer->printer_ref)
    DNSServiceRefDeallocate(printer->printer_ref);
  if (printer->ipp_ref)
    DNSServiceRefDeallocate(printer->ipp_ref);
#  ifdef HAVE_SSL
  if (printer->ipps_ref)
    DNSServiceRefDeallocate(printer->ipps_ref);
#  endif /* HAVE_SSL */
  if (printer->http_ref)
    DNSServiceRefDeallocate(printer->http_ref);

#elif defined(HAVE_AVAHI)
  avahi_threaded_poll_lock(DNSSDMaster);

  if (printer->ipp_ref)
    avahi_entry_group_free(printer->ipp_ref);

  avahi_threaded_poll_unlock(DNSSDMaster);
#endif /* HAVE_DNSSD */

  if (printer->default_uri)
    free(printer->default_uri);
  if (printer->resource)
    free(printer->resource);
  if (printer->dnssd_name)
    free(printer->dnssd_name);
  if (printer->name)
    free(printer->name);
  if (printer->pinfo.icon)
    free(printer->pinfo.icon);
  if (printer->pinfo.command)
    free(printer->pinfo.command);
  if (printer->pinfo.device_uri)
    free(printer->pinfo.device_uri);

  cupsArrayDelete(printer->pinfo.strings);

  ippDelete(printer->pinfo.attrs);
  ippDelete(printer->dev_attrs);

  cupsArrayDelete(printer->active_jobs);
  cupsArrayDelete(printer->completed_jobs);
  cupsArrayDelete(printer->jobs);

  if (printer->identify_message)
    free(printer->identify_message);

  _cupsRWDeinit(&printer->rwlock);

  free(printer);
}


/*
 * 'serverDisablePrinter()' - Stop accepting new jobs for a printer.
 */

void
serverDisablePrinter(
    server_printer_t *printer)		/* I - Printer */
{
  _cupsRWLockWrite(&printer->rwlock);

  printer->is_accepting = 0;

  serverAddEventNoLock(printer, NULL, NULL, SERVER_EVENT_PRINTER_STATE_CHANGED, "No longer accepting jobs.");

  _cupsRWUnlock(&printer->rwlock);
}


/*
 * 'serverEnablePrinter()' - Start accepting new jobs for a printer.
 */

void
serverEnablePrinter(
    server_printer_t *printer)		/* I - Printer */
{
  _cupsRWLockWrite(&printer->rwlock);

  printer->is_accepting = 1;

  serverAddEventNoLock(printer, NULL, NULL, SERVER_EVENT_PRINTER_STATE_CHANGED, "Now accepting jobs.");

  _cupsRWUnlock(&printer->rwlock);
}


/*
 * 'serverGetPrinterStateReasonsBits()' - Get the bits associated with "printer-state-reasons" values.
 */

server_preason_t			/* O - Bits */
serverGetPrinterStateReasonsBits(
    ipp_attribute_t *attr)		/* I - "printer-state-reasons" bits */
{
  int			i, j,		/* Looping vars */
			count;		/* Number of "printer-state-reasons" values */
  const char		*keyword;	/* "printer-state-reasons" value */
  server_preason_t	preasons = SERVER_PREASON_NONE;
					/* Bits for "printer-state-reasons" values */


  count = ippGetCount(attr);
  for (i = 0; i < count; i ++)
  {
    keyword = ippGetString(attr, i, NULL);

    for (j = 0; j < (int)(sizeof(server_preasons) / sizeof(server_preasons[0])); j ++)
    {
      if (!strcmp(keyword, server_preasons[j]))
      {
        preasons |= (server_preason_t)(1 << j);
	break;
      }
    }
  }

  return (preasons);
}


/*
 * 'serverPausePrinter()' - Stop processing jobs for a printer.
 */

void
serverPausePrinter(
    server_printer_t *printer,		/* I - Printer */
    int              immediately)	/* I - Pause immediately? */
{
  _cupsRWLockWrite(&printer->rwlock);

  if (printer->state != IPP_PSTATE_STOPPED)
  {
    if (printer->state == IPP_PSTATE_IDLE)
    {
      printer->state         = IPP_PSTATE_STOPPED;
      printer->state_reasons |= SERVER_PREASON_PAUSED;

      serverAddEventNoLock(printer, NULL, NULL, SERVER_EVENT_PRINTER_STATE_CHANGED |  SERVER_EVENT_PRINTER_STOPPED, "Printer stopped.");
    }
    else if (printer->state == IPP_PSTATE_PROCESSING)
    {
      if (immediately)
	serverStopJob(printer->processing_job);

      printer->state_reasons |= SERVER_PREASON_MOVING_TO_PAUSED;

      serverAddEventNoLock(printer, NULL, NULL, SERVER_EVENT_PRINTER_STATE_CHANGED, "Stopping printer.");
    }
  }

  _cupsRWUnlock(&printer->rwlock);
}


/*
 * 'serverRestartPrinter()' - Restart a printer.
 */

void
serverRestartPrinter(
    server_printer_t *printer)		/* I - Printer */
{
  server_event_t	event = SERVER_EVENT_NONE;
					/* Notification event */


  _cupsRWLockWrite(&printer->rwlock);

  if (!printer->is_accepting)
  {
    printer->is_accepting = 1;
    event                 = SERVER_EVENT_PRINTER_STATE_CHANGED | SERVER_EVENT_PRINTER_RESTARTED;
  }

  if (printer->processing_job)
  {
    serverStopJob(printer->processing_job);

    printer->state_reasons |= SERVER_PREASON_PRINTER_RESTARTED;
    event                  = SERVER_EVENT_PRINTER_STATE_CHANGED;
  }
  else if (printer->state == IPP_PSTATE_STOPPED)
  {
    printer->state         = IPP_PSTATE_IDLE;
    printer->state_reasons = SERVER_PREASON_PRINTER_RESTARTED;
    event                  = SERVER_EVENT_PRINTER_STATE_CHANGED | SERVER_EVENT_PRINTER_RESTARTED;
  }

  if (event)
    serverAddEventNoLock(printer, NULL, NULL, event, printer->state == IPP_PSTATE_IDLE ? "Printer restarted." : "Printer restarting.");

  if (printer->state != IPP_PSTATE_PROCESSING)
    printer->state_reasons &= (server_preason_t)~SERVER_PREASON_PRINTER_RESTARTED;

  _cupsRWUnlock(&printer->rwlock);

  if (printer->state == IPP_PSTATE_IDLE)
    serverCheckJobs(printer);
}


/*
 * 'serverResumePrinter()' - Start processing jobs for a printer.
 */

void
serverResumePrinter(
    server_printer_t *printer)		/* I - Printer */
{
  if (printer->state == IPP_PSTATE_STOPPED)
  {
    _cupsRWLockWrite(&printer->rwlock);

    printer->state         = IPP_PSTATE_IDLE;
    printer->state_reasons &= (server_preason_t)~SERVER_PREASON_PAUSED;

    serverAddEventNoLock(printer, NULL, NULL, SERVER_EVENT_PRINTER_STATE_CHANGED, "Starting printer.");

    _cupsRWUnlock(&printer->rwlock);

    serverCheckJobs(printer);
  }
}


/*
 * 'compare_active_jobs()' - Compare two active jobs.
 */

static int				/* O - Result of comparison */
compare_active_jobs(server_job_t *a,	/* I - First job */
                    server_job_t *b)	/* I - Second job */
{
  int	diff;				/* Difference */


  if ((diff = b->priority - a->priority) == 0)
    diff = b->id - a->id;

  return (diff);
}


/*
 * 'compare_completed_jobs()' - Compare two completed jobs.
 */

static int				/* O - Result of comparison */
compare_completed_jobs(server_job_t *a,	/* I - First job */
                       server_job_t *b)	/* I - Second job */
{
  int	diff;				/* Difference */


  if ((diff = (int)(a->completed - b->completed)) == 0)
    diff = b->id - a->id;

  return (diff);
}


/*
 * 'compare_devices()' - Compare two devices...
 */

static int				/* O - Result of comparison */
compare_devices(server_device_t *a,	/* I - First device */
                server_device_t *b)	/* I - Second device */
{
  return (strcmp(a->uuid, b->uuid));
}


/*
 * 'compare_jobs()' - Compare two jobs.
 */

static int				/* O - Result of comparison */
compare_jobs(server_job_t *a,		/* I - First job */
             server_job_t *b)		/* I - Second job */
{
  return (b->id - a->id);
}


/*
 * 'create_media_col()' - Create a media-col value.
 */

static ipp_t *				/* O - media-col collection */
create_media_col(const char *media,	/* I - Media name */
		 const char *source,	/* I - Media source */
		 const char *type,	/* I - Media type */
		 int        width,	/* I - x-dimension in 2540ths */
		 int        length,	/* I - y-dimension in 2540ths */
		 int        margins)	/* I - Value for margins */
{
  ipp_t	*media_col = ippNew(),		/* media-col value */
	*media_size = create_media_size(width, length);
					/* media-size value */
  char	media_key[256];			/* media-key value */


  if (type && source)
    snprintf(media_key, sizeof(media_key), "%s_%s_%s%s", media, source, type, margins == 0 ? "_borderless" : "");
  else if (type)
    snprintf(media_key, sizeof(media_key), "%s__%s%s", media, type, margins == 0 ? "_borderless" : "");
  else if (source)
    snprintf(media_key, sizeof(media_key), "%s_%s%s", media, source, margins == 0 ? "_borderless" : "");
  else
    snprintf(media_key, sizeof(media_key), "%s%s", media, margins == 0 ? "_borderless" : "");

  ippAddString(media_col, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-key", NULL,
               media_key);
  ippAddCollection(media_col, IPP_TAG_PRINTER, "media-size", media_size);
  ippAddString(media_col, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-size-name", NULL, media);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "media-bottom-margin", margins);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "media-left-margin", margins);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "media-right-margin", margins);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "media-top-margin", margins);
  if (source)
    ippAddString(media_col, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-source", NULL, source);
  if (type)
    ippAddString(media_col, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-type", NULL, type);

  ippDelete(media_size);

  return (media_col);
}


/*
 * 'create_media_size()' - Create a media-size value.
 */

static ipp_t *				/* O - media-col collection */
create_media_size(int width,		/* I - x-dimension in 2540ths */
		  int length)		/* I - y-dimension in 2540ths */
{
  ipp_t	*media_size = ippNew();		/* media-size value */


  ippAddInteger(media_size, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "x-dimension",
                width);
  ippAddInteger(media_size, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "y-dimension",
                length);

  return (media_size);
}


#ifdef HAVE_DNSSD
/*
 * 'dnssd_callback()' - Handle Bonjour registration events.
 */

static void DNSSD_API
dnssd_callback(
    DNSServiceRef       sdRef,		/* I - Service reference */
    DNSServiceFlags     flags,		/* I - Status flags */
    DNSServiceErrorType errorCode,	/* I - Error, if any */
    const char          *name,		/* I - Service name */
    const char          *regtype,	/* I - Service type */
    const char          *domain,	/* I - Domain for service */
    server_printer_t    *printer)	/* I - Printer */
{
  (void)sdRef;
  (void)flags;
  (void)domain;

  if (errorCode)
  {
    fprintf(stderr, "DNSServiceRegister for %s failed with error %d.\n",
            regtype, (int)errorCode);
    return;
  }
  else if (strcasecmp(name, printer->dnssd_name))
  {
    serverLogPrinter(SERVER_LOGLEVEL_INFO, printer, "Now using DNS-SD service name \"%s\".", name);

    /* No lock needed since only the main thread accesses/changes this */
    free(printer->dnssd_name);
    printer->dnssd_name = strdup(name);
  }
}


#elif defined(HAVE_AVAHI)
/*
 * 'dnssd_callback()' - Handle Bonjour registration events.
 */

static void
dnssd_callback(
    AvahiEntryGroup      *srv,		/* I - Service */
    AvahiEntryGroupState state,		/* I - Registration state */
    void                 *context)	/* I - Printer */
{
  (void)srv;
  (void)state;
  (void)context;
}
#endif /* HAVE_DNSSD */


#if defined(HAVE_DNSSD) || defined(HAVE_AVAHI)
/*
 * 'register_geo()' - Register (or update) a printer's geo-location via Bonjour.
 */

static void
register_geo(server_printer_t *printer)	/* I - Printer */
{
  ipp_attribute_t *printer_geo_location;/* printer-geo-location attribute */
  double	lat_degrees = 0.0,	/* Latitude in degrees */
		lon_degrees = 0.0,	/* Longitude in degrees */
		alt_meters = 0.0,	/* Altitude in meters */
		uncertainty = 10.0;	/* Accuracy in meters */
  unsigned	lat_1000ths,		/* Latitude in thousandths of arc seconds */
		lon_1000ths,		/* Longitude in thousandths of arc seconds */
		alt_cmbase;		/* Altitude in centimeters */
  unsigned char	pre;			/* Precision as MSD + power */
  unsigned char	loc[16];		/* LOC record data */


 /*
  * Parse out any geo-location information...
  */

  if ((printer_geo_location = ippFindAttribute(printer->pinfo.attrs, "printer-geo-location", IPP_TAG_URI)))
  {
    char	scheme[32],		/* URI scheme */
		userpass[256],		/* URI username:password */
		host[256],		/* URI hostname */
		resource[1024];		/* URI resource path */
    int		port;			/* URI port */

    if (httpSeparateURI(HTTP_URI_CODING_ALL, ippGetString(printer_geo_location, 0, NULL), scheme, sizeof(scheme), userpass, sizeof(userpass), host, sizeof(host), &port, resource, sizeof(resource)) >= HTTP_URI_STATUS_OK && !strcmp(scheme, "geo"))
    {
     /*
      * Parse "geo:" URI...
      */

      char	*ptr;			/* Pointer into resource */

      lat_degrees = strtod(resource, &ptr);

      if (ptr && *ptr == ',')
      {
        lon_degrees = strtod(ptr, &ptr);

	if (ptr && *ptr == ',')
	  alt_meters = strtod(ptr, &ptr);

        if (ptr && !strncmp(ptr, "?u=", 3))
	  uncertainty = strtod(ptr + 3, NULL);
      }
      else
        lat_degrees = 0.0;
    }
  }

 /*
  * Convert to a DNS LOC record...
  */

  uncertainty *= 100.0;
  pre         = 0;
  while (uncertainty >= 10.0 && pre < 15)
  {
    uncertainty /= 10.0;
    pre ++;
  }

  if (uncertainty >= 10.0)
    pre = 0x9f;
  else
    pre |= (unsigned)uncertainty << 4;

  lat_1000ths = (unsigned)(lat_degrees * 3600000.0) + 2147483648U;
  lon_1000ths = (unsigned)(lon_degrees * 3600000.0) + 2147483648U;
  alt_cmbase  = (unsigned)(alt_meters * 100.0 + 10000000.0);

  loc[0]  = 0;				/* VERSION */
  loc[1]  = 0x51;			/* SIZE = 50cm */
  loc[2]  = pre;			/* HORIZ PRE */
  loc[3]  = pre;			/* VERT PRE */
  loc[4]  = (unsigned char)(lat_1000ths >> 24);
  					/* LATITUDE */
  loc[5]  = (unsigned char)(lat_1000ths >> 16);
  loc[6]  = (unsigned char)(lat_1000ths >> 8);
  loc[7]  = (unsigned char)lat_1000ths;
  loc[8]  = (unsigned char)(lon_1000ths >> 24);
  					/* LONGITUDE */
  loc[9]  = (unsigned char)(lon_1000ths >> 16);
  loc[10] = (unsigned char)(lon_1000ths >> 8);
  loc[11] = (unsigned char)lon_1000ths;
  loc[12] = (unsigned char)(alt_cmbase >> 24);
  					/* ALTITUDE */
  loc[13] = (unsigned char)(alt_cmbase >> 16);
  loc[14] = (unsigned char)(alt_cmbase >> 8);
  loc[15] = (unsigned char)alt_cmbase;

 /*
  * Register the geo-location...
  */

  if (printer->geo_ref)
  {
#ifdef HAVE_DNSSD
    DNSServiceUpdateRecord(printer->ipp_ref, printer->geo_ref, 0, sizeof(loc), loc, 0);

#elif defined(HAVE_AVAHI)
    /* Avahi doesn't support updating */
#endif /* HAVE_DNSSD */
  }
  else
  {
#ifdef HAVE_DNSSD
    DNSServiceAddRecord(printer->ipp_ref, &printer->geo_ref, 0, kDNSServiceType_LOC, sizeof(loc), loc, 0);

#elif defined(HAVE_AVAHI)
    avahi_entry_group_add_record(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, AVAHI_DNS_CLASS_IN, 29, 0, loc, sizeof(loc));
#endif /* HAVE_DNSSD */
  }
}
#endif /* HAVE_DNSSD || HAVE_AVAHI */


/*
 * 'register_printer()' - Register a printer object via Bonjour.
 */

static int				/* O - 1 on success, 0 on error */
register_printer(
    server_printer_t *printer,		/* I - Printer */
    const char       *adminurl,		/* I - Web interface URL */
    const char       *subtype)		/* I - Service subtype */
{
#if defined(HAVE_DNSSD) || defined(HAVE_AVAHI)
  int			is_print3d;	/* 3D printer? */
  server_txt_t		ipp_txt;	/* Bonjour IPP TXT record */
  ipp_attribute_t	*format_sup = ippFindAttribute(printer->pinfo.attrs, "document-format-supported", IPP_TAG_MIMETYPE),
					/* document-formats-supported */
			*kind = ippFindAttribute(printer->pinfo.attrs, "printer-kind", IPP_TAG_KEYWORD),
					/* printer-kind */
			*urf_sup = ippFindAttribute(printer->pinfo.attrs, "urf-supported", IPP_TAG_KEYWORD),
					/* urf-supported */
			*uuid = ippFindAttribute(printer->pinfo.attrs, "printer-uuid", IPP_TAG_URI);
					/* printer-uuid */
  const char		*location,	/* printer-location string */
			*uuidval;	/* String value of UUID */
  int			i,		/* Looping var */
			count;		/* Number for formats */
  char			temp[256],	/* Temporary list */
			*ptr;		/* Pointer into list */
#endif /* HAVE_DNSSD || HAVE_AVAHI */
#ifdef HAVE_DNSSD
  DNSServiceErrorType	error;		/* Error from Bonjour */
  char			make_model[256],/* Make and model together */
			product[256],	/* Product string */
			regtype[256];	/* Bonjour service type */
  server_listener_t	*lis = cupsArrayFirst(Listeners);
					/* Listen socket */


  is_print3d = !strncmp(printer->resource, "/ipp/print3d/", 13);

 /*
  * Build the TXT record for IPP...
  */

  snprintf(make_model, sizeof(make_model), "%s %s", printer->pinfo.make, printer->pinfo.model);
  snprintf(product, sizeof(product), "(%s)", printer->pinfo.model);

  if ((location = printer->pinfo.location) == NULL)
    location = ippGetString(ippFindAttribute(printer->pinfo.attrs, "printer-location", IPP_TAG_TEXT), 0, NULL);

  TXTRecordCreate(&ipp_txt, 1024, NULL);
  TXTRecordSetValue(&ipp_txt, "rp", (uint8_t)strlen(printer->resource) - 1, printer->resource + 1);
  TXTRecordSetValue(&ipp_txt, "ty", (uint8_t)strlen(make_model), make_model);
  TXTRecordSetValue(&ipp_txt, "adminurl", (uint8_t)strlen(adminurl), adminurl);
  if (location && *location)
    TXTRecordSetValue(&ipp_txt, "note", (uint8_t)strlen(location), location);
  if (format_sup)
  {
    for (i = 0, count = ippGetCount(format_sup), ptr = temp; i < count; i ++)
    {
      const char *format = ippGetString(format_sup, i, NULL);

      if (strcmp(format, "application/octet-stream"))
      {
        if (ptr > temp && ptr < (temp + sizeof(temp) - 1))
	  *ptr++ = ',';

	strlcpy(ptr, format, sizeof(temp) - (size_t)(ptr - temp));
	ptr += strlen(ptr);
      }
    }
    *ptr = '\0';

    serverLogPrinter(SERVER_LOGLEVEL_DEBUG, printer, "document-format-supported(%d)=%s", count, temp);
    TXTRecordSetValue(&ipp_txt, "pdl", (uint8_t)strlen(temp), temp);
  }
  if (kind)
  {
    for (i = 0, count = ippGetCount(kind), ptr = temp; i < count; i ++)
    {
      const char *tempkind = ippGetString(kind, i, NULL);

      if (ptr > temp && ptr < (temp + sizeof(temp) - 1))
	*ptr++ = ',';

      strlcpy(ptr, tempkind, sizeof(temp) - (size_t)(ptr - temp));
      ptr += strlen(ptr);
    }
    *ptr = '\0';

    serverLogPrinter(SERVER_LOGLEVEL_DEBUG, printer, "printer-kind(%d)=%s", count, temp);
    TXTRecordSetValue(&ipp_txt, "kind", (uint8_t)strlen(temp), temp);
  }

  if (!is_print3d)
  {
    TXTRecordSetValue(&ipp_txt, "product", (uint8_t)strlen(product), product);
    TXTRecordSetValue(&ipp_txt, "Color", 1, ippGetBoolean(ippFindAttribute(printer->pinfo.attrs, "color-supported", IPP_TAG_BOOLEAN), 0) ? "T" : "F");
    TXTRecordSetValue(&ipp_txt, "Duplex", 1, printer->pinfo.duplex ? "T" : "F");
    if (printer->pinfo.make)
      TXTRecordSetValue(&ipp_txt, "usb_MFG", (uint8_t)strlen(printer->pinfo.make), printer->pinfo.make);
    if (printer->pinfo.model)
      TXTRecordSetValue(&ipp_txt, "usb_MDL", (uint8_t)strlen(printer->pinfo.model), printer->pinfo.model);
  }

  uuidval = ippGetString(uuid, 0, NULL);
  if (uuidval)
  {
    uuidval += 9; /* Skip "urn:uuid:" prefix */

    TXTRecordSetValue(&ipp_txt, "UUID", (uint8_t)strlen(uuidval), uuidval);
  }

#  ifdef HAVE_SSL
  if (!is_print3d && Encryption != HTTP_ENCRYPTION_NEVER)
    TXTRecordSetValue(&ipp_txt, "TLS", 3, "1.2");
#  endif /* HAVE_SSL */

  if (urf_sup)
  {
    for (i = 0, count = ippGetCount(urf_sup), ptr = temp; i < count; i ++)
    {
      const char *val = ippGetString(urf_sup, i, NULL);

      if (ptr > temp && ptr < (temp + sizeof(temp) - 1))
	*ptr++ = ',';

      strlcpy(ptr, val, sizeof(temp) - (size_t)(ptr - temp));
      ptr += strlen(ptr);
    }
    *ptr = '\0';

    serverLogPrinter(SERVER_LOGLEVEL_DEBUG, printer, "urf-supported(%d)=%s", count, temp);
    TXTRecordSetValue(&ipp_txt, "URF", (uint8_t)strlen(temp), temp);
  }

  TXTRecordSetValue(&ipp_txt, "txtvers", 1, "1");
  TXTRecordSetValue(&ipp_txt, "qtotal", 1, "1");

 /*
  * Register the _printer._tcp (LPD) service type with a port number of 0 to
  * defend our service name but not actually support LPD...
  */

  printer->printer_ref = DNSSDMaster;

  if ((error = DNSServiceRegister(&(printer->printer_ref),
                                  kDNSServiceFlagsShareConnection,
                                  0 /* interfaceIndex */, printer->dnssd_name,
				  "_printer._tcp", NULL /* domain */,
				  NULL /* host */, 0 /* port */, 0 /* txtLen */,
				  NULL /* txtRecord */,
			          (DNSServiceRegisterReply)dnssd_callback,
			          printer)) != kDNSServiceErr_NoError)
  {
    serverLogPrinter(SERVER_LOGLEVEL_ERROR, printer, "Unable to register \"%s._printer._tcp\": %d", printer->dnssd_name, error);
    return (0);
  }

 /*
  * Then register the corresponding IPP service types with the real port
  * number to advertise our printer...
  */

  if (!is_print3d)
  {
    printer->ipp_ref = DNSSDMaster;

    if (subtype && *subtype)
      snprintf(regtype, sizeof(regtype), SERVER_IPP_TYPE ",%s", subtype);
    else
      strlcpy(regtype, SERVER_IPP_TYPE, sizeof(regtype));

    if ((error = DNSServiceRegister(&(printer->ipp_ref),
				    kDNSServiceFlagsShareConnection,
				    0 /* interfaceIndex */, printer->dnssd_name,
				    regtype, NULL /* domain */,
				    NULL /* host */, htons(lis->port),
				    TXTRecordGetLength(&ipp_txt),
				    TXTRecordGetBytesPtr(&ipp_txt),
				    (DNSServiceRegisterReply)dnssd_callback,
				    printer)) != kDNSServiceErr_NoError)
    {
      serverLogPrinter(SERVER_LOGLEVEL_ERROR, printer, "Unable to register \"%s.%s\": %d", printer->dnssd_name, regtype, error);
      return (0);
    }
  }

#  ifdef HAVE_SSL
  if (Encryption != HTTP_ENCRYPTION_NEVER)
  {
    printer->ipps_ref = DNSSDMaster;

    if (is_print3d)
    {
      if (subtype && *subtype)
	snprintf(regtype, sizeof(regtype), SERVER_IPPS_3D_TYPE ",%s", subtype);
      else
	strlcpy(regtype, SERVER_IPPS_3D_TYPE, sizeof(regtype));
    }
    else if (subtype && *subtype)
      snprintf(regtype, sizeof(regtype), SERVER_IPPS_TYPE ",%s", subtype);
    else
      strlcpy(regtype, SERVER_IPPS_TYPE, sizeof(regtype));

    if ((error = DNSServiceRegister(&(printer->ipps_ref),
				    kDNSServiceFlagsShareConnection,
				    0 /* interfaceIndex */, printer->dnssd_name,
				    regtype, NULL /* domain */,
				    NULL /* host */, htons(lis->port),
				    TXTRecordGetLength(&ipp_txt),
				    TXTRecordGetBytesPtr(&ipp_txt),
				    (DNSServiceRegisterReply)dnssd_callback,
				    printer)) != kDNSServiceErr_NoError)
    {
      serverLogPrinter(SERVER_LOGLEVEL_ERROR, printer, "Unable to register \"%s.%s\": %d", printer->dnssd_name, regtype, error);
      return (0);
    }
  }
#  endif /* HAVE_SSL */

 /*
  * Register the geolocation of the service...
  */

  register_geo(printer);

 /*
  * Similarly, register the _http._tcp,_printer (HTTP) service type with the
  * real port number to advertise our IPP printer...
  */

  printer->http_ref = DNSSDMaster;

  if ((error = DNSServiceRegister(&(printer->http_ref),
                                  kDNSServiceFlagsShareConnection,
                                  0 /* interfaceIndex */, printer->dnssd_name,
				  SERVER_WEB_TYPE ",_printer", NULL /* domain */,
				  NULL /* host */, htons(lis->port),
				  0 /* txtLen */, NULL, /* txtRecord */
			          (DNSServiceRegisterReply)dnssd_callback,
			          printer)) != kDNSServiceErr_NoError)
  {
    serverLogPrinter(SERVER_LOGLEVEL_ERROR, printer, "Unable to register \"%s.%s\": %d", printer->dnssd_name, SERVER_WEB_TYPE ",_printer", error);
    return (0);
  }

  TXTRecordDeallocate(&ipp_txt);

#elif defined(HAVE_AVAHI)
  server_listener_t	*lis = cupsArrayFirst(Listeners);
					/* Listen socket */


  is_print3d = !strncmp(printer->resource, "/ipp/print3d/", 13);

  if ((location = printer->pinfo.location) == NULL)
    location = ippGetString(ippFindAttribute(printer->pinfo.attrs, "printer-location", IPP_TAG_TEXT), 0, NULL);

 /*
  * Create the TXT record...
  */

  ipp_txt = NULL;
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "rp=%s", printer->resource + 1);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "ty=%s %s", printer->pinfo.make, printer->pinfo.model);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "adminurl=%s", adminurl);
  if (location && *location)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "note=%s", location);
  if (format_sup)
  {
    for (i = 0, count = ippGetCount(format_sup), ptr = temp; i < count; i ++)
    {
      const char *format = ippGetString(format_sup, i, NULL);

      if (strcmp(format, "application/octet-stream"))
      {
        if (ptr > temp && ptr < (temp + sizeof(temp) - 1))
	  *ptr++ = ',';

	strlcpy(ptr, format, sizeof(temp) - (size_t)(ptr - temp));
	ptr += strlen(ptr);
      }
    }
    *ptr = '\0';

    ipp_txt = avahi_string_list_add_printf(ipp_txt, "pdl=%s", temp);
  }
  if (kind)
  {
    for (i = 0, count = ippGetCount(kind), ptr = temp; i < count; i ++)
    {
      const char *tempkind = ippGetString(kind, i, NULL);

      if (ptr > temp && ptr < (temp + sizeof(temp) - 1))
	*ptr++ = ',';

      strlcpy(ptr, tempkind, sizeof(temp) - (size_t)(ptr - temp));
      ptr += strlen(ptr);
    }
    *ptr = '\0';

    serverLogPrinter(SERVER_LOGLEVEL_DEBUG, printer, "printer-kind(%d)=%s", count, temp);
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "kind=%s", temp);
  }

  if (!is_print3d)
  {
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "product=(%s)", printer->pinfo.model);
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "Color=%s", ippGetBoolean(ippFindAttribute(printer->pinfo.attrs, "color-supported", IPP_TAG_BOOLEAN), 0) ? "T" : "F");
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "Duplex=%s", printer->pinfo.duplex ? "T" : "F");
    if (printer->pinfo.make)
      ipp_txt = avahi_string_list_add_printf(ipp_txt, "usb_MFG=%s", printer->pinfo.make);
    if (printer->pinfo.model)
      ipp_txt = avahi_string_list_add_printf(ipp_txt, "usb_MDL=%s", printer->pinfo.model);
  }

  uuidval = ippGetString(uuid, 0, NULL);
  if (uuidval)
  {
    uuidval += 9; /* Skip "urn:uuid:" prefix */

    ipp_txt = avahi_string_list_add_printf(ipp_txt, "UUID=%s", uuidval);
  }

#  ifdef HAVE_SSL
  if (!is_print3d && Encryption != HTTP_ENCRYPTION_NEVER)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "TLS=1.2");
#  endif /* HAVE_SSL */
  if (urf_sup)
  {
    for (i = 0, count = ippGetCount(urf_sup), ptr = temp; i < count; i ++)
    {
      const char *val = ippGetString(urf_sup, i, NULL);

      if (ptr > temp && ptr < (temp + sizeof(temp) - 1))
	*ptr++ = ',';

      strlcpy(ptr, val, sizeof(temp) - (size_t)(ptr - temp));
      ptr += strlen(ptr);
    }
    *ptr = '\0';

    ipp_txt = avahi_string_list_add_printf(ipp_txt, "URF=%s", temp);
  }

  ipp_txt = avahi_string_list_add_printf(ipp_txt, "txtvers=1");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "qtotal=1");

 /*
  * Register _printer._tcp (LPD) with port 0 to reserve the service name...
  */

  avahi_threaded_poll_lock(DNSSDMaster);

  printer->ipp_ref = avahi_entry_group_new(DNSSDClient, dnssd_callback, NULL);

  avahi_entry_group_add_service_strlst(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, "_printer._tcp", NULL, NULL, 0, NULL);

 /*
  * Then register the IPP/IPPS services...
  */

  if (!is_print3d)
  {
    avahi_entry_group_add_service_strlst(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, SERVER_IPP_TYPE, NULL, NULL, lis->port, ipp_txt);
    if (subtype && *subtype)
    {
      snprintf(temp, sizeof(temp), "%s._sub." SERVER_IPP_TYPE, subtype);
      avahi_entry_group_add_service_subtype(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, SERVER_IPP_TYPE, NULL, temp);
    }
  }

#  ifdef HAVE_SSL
  if (Encryption != HTTP_ENCRYPTION_NEVER)
  {
    if (is_print3d)
    {
      avahi_entry_group_add_service_strlst(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, SERVER_IPPS_3D_TYPE, NULL, NULL, lis->port, ipp_txt);
      if (subtype && *subtype)
      {
	snprintf(temp, sizeof(temp), "%s._sub." SERVER_IPPS_3D_TYPE, subtype);
	avahi_entry_group_add_service_subtype(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, SERVER_IPP_TYPE, NULL, temp);
      }
    }
    else
    {
      avahi_entry_group_add_service_strlst(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, SERVER_IPPS_TYPE, NULL, NULL, lis->port, ipp_txt);
      if (subtype && *subtype)
      {
	snprintf(temp, sizeof(temp), "%s._sub." SERVER_IPPS_TYPE, subtype);
	avahi_entry_group_add_service_subtype(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, SERVER_IPP_TYPE, NULL, temp);
      }
    }
  }
#  endif /* HAVE_SSL */

 /*
  * Register the geolocation of the service...
  */

  register_geo(printer);

 /*
  * Finally _http.tcp (HTTP) for the web interface...
  */

  avahi_entry_group_add_service_strlst(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, SERVER_WEB_TYPE, NULL, NULL, lis->port, NULL);
  avahi_entry_group_add_service_subtype(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, SERVER_WEB_TYPE, NULL, "_printer._sub." SERVER_WEB_TYPE);

 /*
  * Commit it...
  */

  avahi_entry_group_commit(printer->ipp_ref);
  avahi_threaded_poll_unlock(DNSSDMaster);

  avahi_string_list_free(ipp_txt);
#endif /* HAVE_DNSSD */

  return (1);
}
