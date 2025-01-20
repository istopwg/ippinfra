//
// IPP Proxy implementation for HP PCL and IPP Everywhere printers.
//
// Copyright © 2016-2025 by the Printer Working Group.
// Copyright © 2014-2018 by Apple Inc.
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more
// information.
//

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#ifndef _WIN32
#  include <signal.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif // !_WIN32
#include <cups/cups.h>
#include <cups/thread.h>


//
// Macros to implement a simple Fibonacci sequence for variable back-off...
//

#define FIB_NEXT(v) (((((v >> 8) + (v & 255) - 1) % 60) + 1) | ((v & 255) << 8))
#define FIB_VALUE(v) (v & 255)


//
// Local types...
//

typedef struct proxy_info_s		// Proxy thread information
{
  bool		done;			// `true` when done

  char		*printer_uri,		// Infrastructure Printer URI
		resource[256];		// Resource path
  ipp_t		*device_attrs;		// Output device attributes
  const char	*device_uri;		// Output device URI
  char		device_uuid[46];	// Device UUID URN
  const char	*outformat;		// Desired output format (`NULL` for auto)

  cups_array_t	*jobs;			// Local jobs
  cups_cond_t	jobs_cond;		// Condition variable to signal changes
  cups_mutex_t	jobs_mutex;		// Mutex for condition variable
  cups_rwlock_t	jobs_rwlock;		// Read/write lock for jobs array
} proxy_info_t;

typedef struct proxy_job_s		// Proxy job information
{
  ipp_jstate_t	local_job_state;	// Local job-state value
  int		local_job_id,		// Local job-id value
		remote_job_id,		// Remote job-id value
		remote_job_state;	// Remote job-state value
} proxy_job_t;


//
// Local globals...
//

static char	*password = NULL;	// Password, if any

static const char * const printer_attrs[] =
		{			// Printer attributes we care about
		  "copies-default",
		  "copies-supported",
		  "document-format-default",
		  "document-format-supported",
		  "finishings-col-database",
		  "finishings-col-default",
		  "finishings-col-ready",
		  "finishings-col-supported",
		  "finishings-default",
		  "finishings-supported",
		  "jpeg-k-octets-supported",
		  "media-bottom-margin-supported",
		  "media-col-database",
		  "media-col-default",
		  "media-col-ready",
		  "media-col-supported",
		  "media-default",
		  "media-left-margin-supported",
		  "media-ready",
		  "media-right-margin-supported",
		  "media-size-supported",
		  "media-source-supported",
		  "media-supported",
		  "media-top-margin-supported",
		  "media-type-supported",
		  "pdf-k-octets-supported",
		  "print-color-mode-default",
		  "print-color-mode-supported",
		  "print-darkness-default",
		  "print-darkness-supported",
		  "print-quality-default",
		  "print-quality-supported",
		  "print-scaling-default",
		  "print-scaling-supported",
		  "printer-darkness-configured",
		  "printer-darkness-supported",
		  "printer-resolution-default",
		  "printer-resolution-supported",
		  "printer-state",
		  "printer-state-reasons",
		  "pwg-raster-document-resolution-supported",
		  "pwg-raster-document-sheet-back",
		  "pwg-raster-document-type-supported",
		  "sides-default",
		  "sides-supported",
		  "urf-supported"
		};
static bool	stop_running = false;
static int	verbosity = 0;


//
// Local functions...
//

static void	acknowledge_identify_printer(http_t *http, proxy_info_t *info);
static bool	attrs_are_equal(ipp_attribute_t *a, ipp_attribute_t *b);
static int	compare_jobs(proxy_job_t *a, proxy_job_t *b);
static ipp_t	*create_media_col(const char *media, const char *source, const char *type, int width, int length, int margins);
static ipp_t	*create_media_size(int width, int length);
static void	deregister_printer(http_t *http, proxy_info_t *info, int subscription_id);
static proxy_job_t *find_job(proxy_info_t *info, int remote_job_id);
static ipp_t	*get_device_attrs(const char *device_uri);
static void	make_uuid(const char *device_uri, char *uuid, size_t uuidsize);
static const char *password_cb(const char *prompt, http_t *http, const char *method, const char *resource, void *user_data);
static void	plogipp(proxy_job_t *pjob, bool is_request, ipp_t *ipp);
static void	plogf(proxy_job_t *pjob, const char *message, ...);
static void	*proxy_jobs(proxy_info_t *info);
static int	register_printer(http_t **http, proxy_info_t *info);
static void	run_job(proxy_info_t *info, proxy_job_t *pjob);
static void	run_printer(http_t *http, proxy_info_t *info, int subscription_id);
static void	send_document(http_t *http, proxy_info_t *info, proxy_job_t *pjob, ipp_t *job_attrs, ipp_t *doc_attrs, int doc_number);
static void	sighandler(int sig);
static bool	update_device_attrs(http_t *http, proxy_info_t *info, ipp_t *new_attrs);
static void	update_document_status(http_t *http, proxy_info_t *info, proxy_job_t *pjob, int doc_number, ipp_dstate_t doc_state);
static void	update_job_status(http_t *http, proxy_info_t *info, proxy_job_t *pjob);
static bool	update_remote_jobs(http_t *http, proxy_info_t *info);
static int	usage(FILE *out);


//
// 'main()' - Main entry for ippproxy.
//

int					// O - Exit status
main(int  argc,				// I - Number of command-line arguments
     char *argv[])			// I - Command-line arguments
{
  int		i;			// Looping var
  char		*opt;			// Current option
  http_t	*http;			// Connection to printer
  int		subscription_id;	// Event subscription ID
  unsigned	interval = 1;		// Current retry interval
  proxy_info_t	info;			// Proxy information


  // Initialize the proxy information
  memset(&info, 0, sizeof(info));

  cupsRWInit(&info.jobs_rwlock);
  cupsMutexInit(&info.jobs_mutex);
  cupsCondInit(&info.jobs_cond);

  // Parse command-line...
  for (i = 1; i < argc; i ++)
  {
    if (argv[i][0] == '-' && argv[i][1] == '-')
    {
      if (!strcmp(argv[i], "--help"))
      {
        return (usage(stdout));
      }
      else if (!strcmp(argv[i], "--version"))
      {
        puts(IPPSAMPLE_VERSION);
        return (0);
      }
      else
      {
        fprintf(stderr, "ippproxy: Unknown option '%s'.\n", argv[i]);
	return (usage(stderr));
      }
    }
    else if (argv[i][0] == '-')
    {
      for (opt = argv[i] + 1; *opt; opt ++)
      {
        switch (*opt)
	{
	  case 'd' : // -d device-uri
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ippproxy: Missing device URI after '-d' option.\n", stderr);
		return (usage(stderr));
	      }

	      if (strncmp(argv[i], "ipp://", 6) && strncmp(argv[i], "ipps://", 7) && strncmp(argv[i], "socket://", 9))
	      {
	        fputs("ippproxy: Unsupported device URI scheme.\n", stderr);
		return (usage(stderr));
	      }

	      info.device_uri = argv[i];
	      break;

          case 'm' : // -m mime/type
              i ++;
              if (i >= argc)
	      {
	        fputs("ippproxy: Missing MIME media type after '-m' option.\n", stderr);
		return (usage(stderr));
	      }

	      info.outformat = argv[i];
	      break;

	  case 'p' : // -p password
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ippproxy: Missing password after '-p' option.\n", stderr);
		return (usage(stderr));
	      }

	      password = argv[i];
	      break;

	  case 'u' : // -u user
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ippproxy: Missing username after '-u' option.\n", stderr);
		return (usage(stderr));
	      }

	      cupsSetUser(argv[i]);
	      break;

          case 'v' : // Be verbose
              verbosity ++;
              break;

	  default :
	      fprintf(stderr, "ippproxy: Unknown option '-%c'.\n", *opt);
	      return (usage(stderr));
	}
      }
    }
    else if (info.printer_uri)
    {
      fprintf(stderr, "ippproxy: Unexpected option '%s'.\n", argv[i]);
      return (usage(stderr));
    }
    else
    {
      info.printer_uri = strdup(argv[i]);
    }
  }

  if (!info.printer_uri)
    return (usage(stderr));

  if (!info.device_uri)
  {
    fputs("ippproxy: Must specify '-d device-uri'.\n", stderr);
    return (usage(stderr));
  }

  if (!password)
    password = getenv("IPPPROXY_PASSWORD");

  if (password)
    cupsSetPasswordCB(password_cb, password);

  make_uuid(info.device_uri, info.device_uuid, sizeof(info.device_uuid));

  // Connect to the infrastructure printer...
  if (verbosity)
    plogf(NULL, "Main thread connecting to '%s'.", info.printer_uri);

  while ((http = httpConnectURI(info.printer_uri, /*host*/NULL, /*hsize*/0, /*port*/NULL, info.resource, sizeof(info.resource), /*blocking*/true, /*msec*/30000, /*cancel*/NULL, /*require_ca*/false)) == NULL)
  {
    interval = FIB_NEXT(interval);

    plogf(NULL, "'%s' is not responding, retrying in %u seconds.", info.printer_uri, FIB_VALUE(interval));
    sleep(FIB_VALUE(interval));
  }

  if (verbosity)
    plogf(NULL, "Connected to '%s'.", info.printer_uri);

  // Register the printer and wait for jobs to process...
#ifndef _WIN32
  signal(SIGHUP, sighandler);
  signal(SIGINT, sighandler);
  signal(SIGTERM, sighandler);
#endif // !_WIN32

  if ((subscription_id = register_printer(&http, &info)) == 0)
  {
    httpClose(http);
    return (1);
  }

  run_printer(http, &info, subscription_id);

  deregister_printer(http, &info, subscription_id);

  httpClose(http);

  return (0);
}


//
// 'acknowledge_identify_printer()' - Acknowledge an Identify-Printer request.
//

static void
acknowledge_identify_printer(
    http_t       *http,			// I - HTTP connection
    proxy_info_t *info)			// I - Proxy information
{
  ipp_t		*request,		// IPP request
		*response;		// IPP response
  ipp_attribute_t *actions,		// "identify-actions" attribute
		*message;		// "message" attribute


  request = ippNewRequest(IPP_OP_ACKNOWLEDGE_IDENTIFY_PRINTER);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "device-uuid", NULL, info->device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  response = cupsDoRequest(http, request, info->resource);

  actions = ippFindAttribute(response, "identify-actions", IPP_TAG_KEYWORD);
  message = ippFindAttribute(response, "message", IPP_TAG_TEXT);

  if (ippContainsString(actions, "display"))
    printf("IDENTIFY-PRINTER: display (%s)\n", message ? ippGetString(message, 0, NULL) : "No message supplied");

  if (!actions || ippContainsString(actions, "sound"))
    puts("IDENTIFY-PRINTER: sound\007");

  ippDelete(response);
}


//
// 'attrs_are_equal()' - Compare two attributes for equality.
//

static bool				// O - `true` if equal, `false` otherwise
attrs_are_equal(ipp_attribute_t *a,	// I - First attribute
                ipp_attribute_t *b)	// I - Second attribute
{
  size_t	i,			// Looping var
		count;			// Number of values
  ipp_tag_t	tag;			// Type of value


  // Check that both 'a' and 'b' point to something first...
  if ((a != NULL) != (b != NULL))
    return (false);

  if (a == NULL && b == NULL)
    return (true);

  // Check that 'a' and 'b' are of the same type with the same number of values...
  if ((tag = ippGetValueTag(a)) != ippGetValueTag(b))
    return (false);

  if ((count = ippGetCount(a)) != ippGetCount(b))
    return (false);

  // Compare values...
  switch (tag)
  {
    case IPP_TAG_INTEGER :
    case IPP_TAG_ENUM :
        for (i = 0; i < count; i ++)
        {
	  if (ippGetInteger(a, i) != ippGetInteger(b, i))
	    return (false);
	}
	break;

    case IPP_TAG_BOOLEAN :
        for (i = 0; i < count; i ++)
	{
	  if (ippGetBoolean(a, i) != ippGetBoolean(b, i))
	    return (false);
	}
	break;

    case IPP_TAG_KEYWORD :
        for (i = 0; i < count; i ++)
        {
	  if (strcmp(ippGetString(a, i, NULL), ippGetString(b, i, NULL)))
	    return (false);
	}
	break;

    default :
	return (false);
  }

  // If we get this far we must be the same...
  return (true);
}


//
// 'compare_jobs()' - Compare two jobs.
//

static int
compare_jobs(proxy_job_t *a,		// I - First job
             proxy_job_t *b)		// I - Second job
{
  return (a->remote_job_id - b->remote_job_id);
}


//
// 'create_media_col()' - Create a media-col value.
//

static ipp_t *				// O - media-col collection
create_media_col(const char *media,	// I - Media name
		 const char *source,	// I - Media source
		 const char *type,	// I - Media type
		 int        width,	// I - x-dimension in 2540ths
		 int        length,	// I - y-dimension in 2540ths
		 int        margins)	// I - Value for margins
{
  ipp_t	*media_col = ippNew(),		// media-col value
	*media_size = create_media_size(width, length);
					// media-size value
  char	media_key[256];			// media-key value


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


//
// 'create_media_size()' - Create a media-size value.
//

static ipp_t *				// O - media-col collection
create_media_size(int width,		// I - x-dimension in 2540ths
		  int length)		// I - y-dimension in 2540ths
{
  ipp_t	*media_size = ippNew();		// media-size value


  ippAddInteger(media_size, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "x-dimension",
                width);
  ippAddInteger(media_size, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "y-dimension",
                length);

  return (media_size);
}


//
// 'deregister_printer()' - Unregister the output device and cancel the printer subscription.
//

static void
deregister_printer(
    http_t       *http,			// I - Connection to printer
    proxy_info_t *info,			// I - Proxy information
    int          subscription_id)	// I - Subscription ID
{
  ipp_t	*request;			// IPP request


  // Cancel the subscription we are using...
  request = ippNewRequest(IPP_OP_CANCEL_SUBSCRIPTION);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "notify-subscription-id", subscription_id);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  ippDelete(cupsDoRequest(http, request, info->resource));

  // Then deregister the output device...
  request = ippNewRequest(IPP_OP_DEREGISTER_OUTPUT_DEVICE);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  ippDelete(cupsDoRequest(http, request, info->resource));
}


//
// 'find_job()' - Find a remote job that has been queued for proxying...
//

static proxy_job_t *			// O - Proxy job or @code NULL@ if not found
find_job(proxy_info_t *info,		// I - Proxy info
         int          remote_job_id)	// I - Remote job ID
{
  proxy_job_t	key,			// Search key
		*match;			// Matching job, if any


  key.remote_job_id = remote_job_id;

  cupsRWLockRead(&info->jobs_rwlock);
  match = (proxy_job_t *)cupsArrayFind(info->jobs, &key);
  cupsRWUnlock(&info->jobs_rwlock);

  return (match);
}


//
// 'get_device_attrs()' - Get current attributes for a device.
//

static ipp_t *				// O - IPP attributes
get_device_attrs(const char *device_uri)// I - Device URI
{
  ipp_t	*response = NULL;		// IPP attributes


  if (!strncmp(device_uri, "ipp://", 6) || !strncmp(device_uri, "ipps://", 7))
  {
    // Query the IPP printer...
    size_t	i,			// Looping var
		count;			// Number of values
    http_t	*http;			// Connection to printer
    char	resource[1024];		// Resource path
    ipp_t	*request;		// Get-Printer-Attributes request
    ipp_attribute_t *urf_supported,	// urf-supported
		*pwg_supported;		// pwg-raster-document-xxx-supported
    unsigned	interval = 1;		// Current retry interval

    // Connect to the printer...
    while ((http = httpConnectURI(device_uri, /*host*/NULL, /*hsize*/0, /*port*/NULL, resource, sizeof(resource), /*blocking*/true, /*msec*/30000, /*cancel*/NULL, /*require_ca*/false)) == NULL)
    {
      interval = FIB_NEXT(interval);

      plogf(NULL, "'%s' is not responding, retrying in %u seconds.", device_uri, FIB_VALUE(interval));
      sleep(FIB_VALUE(interval));
    }

    // Get the attributes...
    request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, device_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", (int)(sizeof(printer_attrs) / sizeof(printer_attrs[0])), NULL, printer_attrs);

    response = cupsDoRequest(http, request, resource);

    if (cupsGetError() >= IPP_STATUS_ERROR_BAD_REQUEST)
    {
      fprintf(stderr, "ippproxy: Device at '%s' returned error: %s\n", device_uri, cupsGetErrorString());
      ippDelete(response);
      response = NULL;
    }

    httpClose(http);

    // Convert urf-supported to pwg-raster-document-xxx-supported, as needed...
    urf_supported = ippFindAttribute(response, "urf-supported", IPP_TAG_KEYWORD);
    pwg_supported = ippFindAttribute(response, "pwg-raster-document-resolution-supported", IPP_TAG_RESOLUTION);
    if (urf_supported && !pwg_supported)
    {
      for (i = 0, count = ippGetCount(urf_supported); i < count; i ++)
      {
        const char *keyword = ippGetString(urf_supported, i, NULL);
					// Value from urf_supported

        if (!strncmp(keyword, "RS", 2))
        {
	  char	*ptr;			// Pointer into value
	  int	res;			// Resolution

          for (res = (int)strtol(keyword + 2, &ptr, 10); res > 0; res = (int)strtol(ptr + 1, &ptr, 10))
	  {
	    if (pwg_supported)
	      ippSetResolution(response, &pwg_supported, ippGetCount(pwg_supported), IPP_RES_PER_INCH, res, res);
	    else
	      pwg_supported = ippAddResolution(response, IPP_TAG_PRINTER, "pwg-raster-document-resolution-supported", IPP_RES_PER_INCH, res, res);
	  }
        }
      }
    }

    pwg_supported = ippFindAttribute(response, "pwg-raster-document-sheet-back", IPP_TAG_KEYWORD);
    if (urf_supported && !pwg_supported)
    {
      for (i = 0, count = ippGetCount(urf_supported); i < count; i ++)
      {
        const char *keyword = ippGetString(urf_supported, i, NULL);
					// Value from urf_supported

        if (!strncmp(keyword, "DM", 2))
        {
          if (!strcmp(keyword, "DM1"))
            pwg_supported = ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", NULL, "normal");
          else if (!strcmp(keyword, "DM2"))
            pwg_supported = ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", NULL, "flipped");
          else if (!strcmp(keyword, "DM3"))
            pwg_supported = ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", NULL, "rotated");
          else
            pwg_supported = ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", NULL, "manual-tumble");
        }
      }
    }

    pwg_supported = ippFindAttribute(response, "pwg-raster-document-type-supported", IPP_TAG_KEYWORD);
    if (urf_supported && !pwg_supported)
    {
      for (i = 0, count = ippGetCount(urf_supported); i < count; i ++)
      {
        const char *keyword = ippGetString(urf_supported, i, NULL);
					// Value from urf_supported
        const char *pwg_keyword = NULL;	// Value for pwg-raster-document-type-supported

        if (!strcmp(keyword, "ADOBERGB24"))
          pwg_keyword = "adobe-rgb_8";
	else if (!strcmp(keyword, "ADOBERGB48"))
          pwg_keyword = "adobe-rgb_16";
	else if (!strcmp(keyword, "SRGB24"))
          pwg_keyword = "srgb_8";
	else if (!strcmp(keyword, "W8"))
          pwg_keyword = "sgray_8";
	else if (!strcmp(keyword, "W16"))
          pwg_keyword = "sgray_16";

        if (pwg_keyword)
        {
	  if (pwg_supported)
	    ippSetString(response, &pwg_supported, ippGetCount(pwg_supported), pwg_keyword);
	  else
	    pwg_supported = ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-type-supported", NULL, pwg_keyword);
        }
      }
    }
  }
  else
  {
    // Must be a socket-based HP PCL laser printer, report just standard size information...
    size_t		i;		// Looping var
    ipp_attribute_t	*media_col_database,
					// media-col-database value
			*media_size_supported;
					// media-size-supported value
    ipp_t		*media_col;	// media-col-default value
    static const int media_col_sizes[][2] =
    {					// Default media-col sizes
      { 21590, 27940 },			// Letter
      { 21590, 35560 },			// Legal
      { 21000, 29700 }			// A4
    };
    static const char * const media_col_supported[] =
    {					// media-col-supported values
      "media-bottom-margin",
      "media-left-margin",
      "media-right-margin",
      "media-size",
      "media-size-name",
      "media-top-margin"
    };
    static const char * const media_supported[] =
    {					// Default media sizes
      "na_letter_8.5x11in",		// Letter
      "na_legal_8.5x14in",		// Legal
      "iso_a4_210x297mm"			// A4
    };
    static const int quality_supported[] =
    {					// print-quality-supported values
      IPP_QUALITY_DRAFT,
      IPP_QUALITY_NORMAL,
      IPP_QUALITY_HIGH
    };
    static const int resolution_supported[] =
    {					// printer-resolution-supported values
      300,
      600
    };
    static const char * const sides_supported[] =
    {					// sides-supported values
      "one-sided",
      "two-sided-long-edge",
      "two-sided-short-edge"
    };

    response = ippNew();

    ippAddRange(response, IPP_TAG_PRINTER, "copies-supported", 1, 1);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE, "document-format-supported", NULL, "application/vnd.hp-pcl");
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-bottom-margin-supported", 635);

    media_col_database = ippAddCollections(response, IPP_TAG_PRINTER, "media-col-database", (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0])), NULL);
    for (i = 0; i < (sizeof(media_col_sizes) / sizeof(media_col_sizes[0])); i ++)
    {
      media_col = create_media_col(media_supported[i], NULL, NULL, media_col_sizes[i][0], media_col_sizes[i][1], 635);

      ippSetCollection(response, &media_col_database, i, media_col);

      ippDelete(media_col);
    }

    media_col = create_media_col(media_supported[0], NULL, NULL, media_col_sizes[0][0], media_col_sizes[0][1], 635);
    ippAddCollection(response, IPP_TAG_PRINTER, "media-col-default", media_col);
    ippDelete(media_col);

    media_col = create_media_col(media_supported[0], NULL, NULL, media_col_sizes[0][0], media_col_sizes[0][1], 635);
    ippAddCollection(response, IPP_TAG_PRINTER, "media-col-ready", media_col);
    ippDelete(media_col);

    ippAddStrings(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-col-supported", sizeof(media_col_supported) / sizeof(media_col_supported[0]), NULL, media_col_supported);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-default", NULL, media_supported[0]);
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-left-margin-supported", 635);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-ready", NULL, media_supported[0]);
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-right-margin-supported", 635);

    media_size_supported = ippAddCollections(response, IPP_TAG_PRINTER, "media-size-supported", sizeof(media_col_sizes) / sizeof(media_col_sizes[0]), NULL);
    for (i = 0; i < (sizeof(media_col_sizes) / sizeof(media_col_sizes[0])); i ++)
    {
      ipp_t *size = create_media_size(media_col_sizes[i][0], media_col_sizes[i][1]);

      ippSetCollection(response, &media_size_supported, i, size);
      ippDelete(size);
    }

    ippAddStrings(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-supported", (int)(sizeof(media_supported) / sizeof(media_supported[0])), NULL, media_supported);
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-top-margin-supported", 635);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "print-color-mode-default", NULL, "monochrome");
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "print-color-mode-supported", NULL, "monochrome");
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_ENUM, "print-quality-default", IPP_QUALITY_NORMAL);
    ippAddIntegers(response, IPP_TAG_PRINTER, IPP_TAG_ENUM, "print-quality-supported", (int)(sizeof(quality_supported) / sizeof(quality_supported[0])), quality_supported);
    ippAddResolution(response, IPP_TAG_PRINTER, "printer-resolution-default", IPP_RES_PER_INCH, 300, 300);
    ippAddResolutions(response, IPP_TAG_PRINTER, "printer-resolution-supported", sizeof(resolution_supported) / sizeof(resolution_supported[0]), IPP_RES_PER_INCH, resolution_supported, resolution_supported);
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state", IPP_PSTATE_IDLE);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "printer-state-reasons", NULL, "none");
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "sides-default", NULL, "two-sided-long-edge");
    ippAddStrings(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "sides-supported", sizeof(sides_supported) / sizeof(sides_supported[0]), NULL, sides_supported);
  }

  return (response);
}


//
// 'make_uuid()' - Make a RFC 4122 URN UUID from the device URI.
//
// NULL device URIs are (appropriately) mapped to "file://hostname/dev/null".
//

static void
make_uuid(const char *device_uri,	// I - Device URI or NULL
          char       *uuid,		// I - UUID string buffer
	  size_t     uuidsize)		// I - Size of UUID buffer
{
  char			nulluri[1024];	// NULL URI buffer
  unsigned char		sha256[32];	// SHA-256 hash


  // Use "file://hostname/dev/null" if the device URI is NULL...
  if (!device_uri)
  {
    char	host[1024];		// Hostname

    httpGetHostname(NULL, host, sizeof(host));
    httpAssembleURI(HTTP_URI_CODING_ALL, nulluri, sizeof(nulluri), "file", NULL, host, 0, "/dev/null");
    device_uri = nulluri;
  }

  // Build a version 3 UUID conforming to RFC 4122 based on the SHA-256 hash of the device URI.
  cupsHashData("sha2-256", device_uri, strlen(device_uri), sha256, sizeof(sha256));

  snprintf(uuid, uuidsize, "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", sha256[16], sha256[17], sha256[18], sha256[19], sha256[20], sha256[21], (sha256[22] & 15) | 0x30, sha256[23], (sha256[24] & 0x3f) | 0x40, sha256[25], sha256[26], sha256[27], sha256[28], sha256[29], sha256[30], sha256[31]);

  if (verbosity)
    plogf(NULL, "UUID for '%s' is '%s'.", device_uri, uuid);
}


//
// 'password_cb()' - Password callback.
//

static const char *			// O - Password string
password_cb(const char *prompt,		// I - Prompt (unused)
            http_t     *http,		// I - Connection (unused)
	    const char *method,		// I - Method (unused)
	    const char *resource,	// I - Resource path (unused)
	    void       *user_data)	// I - Password string
{
  (void)prompt;
  (void)http;
  (void)method;
  (void)resource;

  return ((char *)user_data);
}


//
// 'plogipp()' - Log an IPP message to stderr.
//

static void
plogipp(proxy_job_t *pjob,		// I - Proxy job, if any
        bool        is_request,		// I - Request message?
        ipp_t       *ipp)		// I - IPP message
{
  int		minor, major = ippGetVersion(ipp, &minor);
					// IPP version
  ipp_attribute_t *attr;		// Current attribute
  ipp_tag_t	prev_group_tag,		// Previous group tag
		group_tag,		// Current group tag
		value_tag;		// Current value tag
  const char	*name;			// Attribute name
  size_t	count;			// Number of values
  char		value[4096];		// Attribute value
  const char	*prefix = pjob ? "" : "[Printer] ";
					// Prefix string


  if (is_request)
    plogf(pjob, "%s%s %d IPP/%d.%d", prefix, ippOpString(ippGetOperation(ipp)), ippGetRequestId(ipp), major, minor);
  else
    plogf(pjob, "%s%s %d IPP/%d.%d", prefix, ippErrorString(ippGetStatusCode(ipp)), ippGetRequestId(ipp), major, minor);

  for (attr = ippGetFirstAttribute(ipp), prev_group_tag = IPP_TAG_ZERO; attr; attr = ippGetNextAttribute(ipp))
  {
    if ((name = ippGetName(attr)) == NULL)
    {
      // Separator...
      prev_group_tag = IPP_TAG_ZERO;
      continue;
    }

    group_tag = ippGetGroupTag(attr);
    value_tag = ippGetValueTag(attr);
    count     = ippGetCount(attr);

    ippAttributeString(attr, value, sizeof(value));

    if (group_tag != prev_group_tag)
    {
      plogf(pjob, "%s  ---- %s ----", prefix, ippTagString(group_tag));
      prev_group_tag = group_tag;
    }

    plogf(pjob, "%s  %s %s%s %s", prefix, name, count > 1 ? "1setOf " : "", ippTagString(value_tag), value);
  }

  plogf(pjob, "%s  ---- end-of-attributes-tag ----", prefix);
}


//
// 'plogf()' - Log a message to stderr.
//

static void
plogf(proxy_job_t *pjob,			// I - Proxy job, if any
      const char  *message,		// I - Message
      ...)				// I - Additional arguments as needed
{
  char		temp[1024];		// Temporary message string
  va_list	ap;			// Pointer to additional arguments
  struct timeval curtime;		// Current time
  struct tm	curdate;		// Current date and time


#ifdef _WIN32
  _cups_gettimeofday(&curtime, NULL);
  time_t tv_sec = (time_t)curtime.tv_sec;
  gmtime_s(&curdate, &tv_sec);
#else
  gettimeofday(&curtime, NULL);
  gmtime_r(&curtime.tv_sec, &curdate);
#endif // _WIN32

  if (pjob)
    snprintf(temp, sizeof(temp), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ  [Job %d] %s\n", curdate.tm_year + 1900, curdate.tm_mon + 1, curdate.tm_mday, curdate.tm_hour, curdate.tm_min, curdate.tm_sec, (int)curtime.tv_usec / 1000, pjob->remote_job_id, message);
  else
    snprintf(temp, sizeof(temp), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ  %s\n", curdate.tm_year + 1900, curdate.tm_mon + 1, curdate.tm_mday, curdate.tm_hour, curdate.tm_min, curdate.tm_sec, (int)curtime.tv_usec / 1000, message);

  va_start(ap, message);
  vfprintf(stderr, temp, ap);
  va_end(ap);
}


//
// 'proxy_jobs()' - Relay jobs to the local printer.
//

static void *				// O - Thread exit status
proxy_jobs(proxy_info_t *info)		// I - Printer and device info
{
  proxy_job_t	*pjob;			// Current job


  plogf(NULL, "proxy_jobs: info              = %p", (void *)info);
  plogf(NULL, "proxy_jobs: info->done        = %s", info->done ? "true" : "false");
  plogf(NULL, "proxy_jobs: info->printer_uri = \"%s\"", info->printer_uri);
  plogf(NULL, "proxy_jobs: info->resource    = \"%s\"", info->resource);
  plogf(NULL, "proxy_jobs: info->device_uri  = \"%s\"", info->device_uri);
  plogf(NULL, "proxy_jobs: info->device_uuid = \"%s\"", info->device_uuid);
  plogf(NULL, "proxy_jobs: info->outformat   = \"%s\"", info->outformat);

  // Connect to the infrastructure printer...
  if (verbosity)
    plogf(NULL, "Job processing thread starting.");

  if (password)
    cupsSetPasswordCB(password_cb, password);

  cupsMutexLock(&info->jobs_mutex);

  while (!info->done)
  {
    // Look for a fetchable job...
    if (verbosity)
      plogf(NULL, "Checking for queued jobs.");

    cupsRWLockRead(&info->jobs_rwlock);
    for (pjob = (proxy_job_t *)cupsArrayGetFirst(info->jobs); pjob; pjob = (proxy_job_t *)cupsArrayGetNext(info->jobs))
    {
      if (pjob->local_job_state == IPP_JSTATE_PENDING && pjob->remote_job_state < IPP_JSTATE_CANCELED)
        break;
    }
    cupsRWUnlock(&info->jobs_rwlock);

    if (pjob)
    {
      // Process this job...
      run_job(info, pjob);
    }
    else
    {
      // We didn't have a fetchable job so purge the job cache and wait for more jobs...
      cupsRWLockWrite(&info->jobs_rwlock);
      for (pjob = (proxy_job_t *)cupsArrayGetFirst(info->jobs); pjob; pjob = (proxy_job_t *)cupsArrayGetNext(info->jobs))
      {
	if (pjob->remote_job_state >= IPP_JSTATE_CANCELED)
	  cupsArrayRemove(info->jobs, pjob);
      }
      cupsRWUnlock(&info->jobs_rwlock);

      if (verbosity)
        plogf(NULL, "Waiting for jobs.");

      cupsCondWait(&info->jobs_cond, &info->jobs_mutex, 15.0);
    }
  }

  cupsMutexUnlock(&info->jobs_mutex);

  return (NULL);
}


//
// 'register_printer()' - Register the printer (output device) with the Infrastructure Printer.
//

static int				// O - Subscription ID
register_printer(
    http_t       **http,		// I - Connection to printer
    proxy_info_t *info)			// I - Proxy information
{
  ipp_t		*request,		// IPP request
		*response;		// IPP response
  ipp_attribute_t *attr;		// Attribute in response
  int		subscription_id = 0;	// Subscription ID
  static const char * const events[] =	// Events to monitor
  {
    "document-config-changed",
    "document-state-changed",
    "job-config-changed",
    "job-fetchable",
    "job-state-changed",
    "printer-config-changed",
    "printer-state-changed"
  };


  // If we are talking to a system service (/ipp/system), then register the
  // output device to get a printer URI...
  if (!strcmp(info->resource, "/ipp/system"))
  {
    ipp_attribute_t	*printer_xri;	// printer-xri-supported for the printer
    const char		*xri_uri;	// xri-uri for the printer

    request = ippNewRequest(IPP_OP_REGISTER_OUTPUT_DEVICE);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "system-uri", NULL, info->printer_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "printer-service-type", NULL, "print");

    if (verbosity)
      plogipp(/*pjob*/NULL, /*is_request*/true, request);

    response = cupsDoRequest(*http, request, info->resource);

    if (verbosity)
      plogipp(/*pjob*/NULL, /*is_request*/false, response);

    if (cupsGetError() >= IPP_STATUS_ERROR_BAD_REQUEST)
    {
      plogf(NULL, "Unable to register the output device: %s(%s)", ippErrorString(cupsGetError()), cupsGetErrorString());
      ippDelete(response);
      return (0);
    }

    if ((printer_xri = ippFindAttribute(response, "printer-xri-supported", IPP_TAG_BEGIN_COLLECTION)) == NULL)
    {
      plogf(NULL, "No print service XRI returned for output device.");
      ippDelete(response);
      return (0);
    }

    if ((xri_uri = ippGetString(ippFindAttribute(ippGetCollection(printer_xri, 0), "xri-uri", IPP_TAG_URI), 0, NULL)) == NULL)
    {
      plogf(NULL, "No print service URI returned for output device.");
      ippDelete(response);
      return (0);
    }

    plogf(NULL, "Registered printer-uri is '%s'.", xri_uri);

    free(info->printer_uri);
    info->printer_uri = strdup(xri_uri);

    ippDelete(response);
    httpClose(*http);

    if ((*http = httpConnectURI(info->printer_uri, /*host*/NULL, /*hsize*/0, /*port*/NULL, info->resource, sizeof(info->resource), /*blocking*/true, /*msec*/30000, /*cancel*/NULL, /*require_ca*/false)) == NULL)
    {
      plogf(NULL, "Unable to connect to '%s': %s", info->printer_uri, cupsGetErrorString());
      return (0);
    }
  }

  // Create a printer subscription to monitor for events...
  request = ippNewRequest(IPP_OP_CREATE_PRINTER_SUBSCRIPTIONS);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  ippAddString(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD, "notify-pull-method", NULL, "ippget");
  ippAddStrings(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD, "notify-events", (int)(sizeof(events) / sizeof(events[0])), NULL, events);
  ippAddInteger(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER, "notify-lease-duration", 0);

  if (verbosity)
    plogipp(/*pjob*/NULL, /*is_request*/true, request);

  response = cupsDoRequest(*http, request, info->resource);

  if (verbosity)
    plogipp(/*pjob*/NULL, /*is_request*/false, response);

  if (cupsGetError() != IPP_STATUS_OK)
  {
    plogf(NULL, "Unable to monitor events on '%s': %s", info->printer_uri, cupsGetErrorString());
    return (0);
  }

  if ((attr = ippFindAttribute(response, "notify-subscription-id", IPP_TAG_INTEGER)) != NULL)
  {
    subscription_id = ippGetInteger(attr, 0);

    if (verbosity)
      plogf(NULL, "Monitoring events with subscription #%d.", subscription_id);
  }
  else
  {
    plogf(NULL, "Unable to monitor events on '%s': No notify-subscription-id returned.", info->printer_uri);
  }

  ippDelete(response);

  return (subscription_id);
}


//
// 'run_job()' - Fetch and print a job.
//

static void
run_job(proxy_info_t *info,		// I - Proxy information
        proxy_job_t  *pjob)		// I - Proxy job to fetch and print
{
  http_t	*http;			// HTTP connection
  bool		first_time = true;	// First time connecting?
  ipp_t		*request,		// IPP request
		*job_attrs,		// Job attributes
		*doc_attrs;		// Document attributes
  int		num_docs,		// Number of documents
		doc_number;		// Current document number
  ipp_attribute_t *doc_formats;		// Supported document formats
  const char	*doc_format = NULL;	// Document format we want...


  // Figure out the output format we want to use...
  doc_formats = ippFindAttribute(info->device_attrs, "document-format-supported", IPP_TAG_MIMETYPE);

  if (info->outformat)
  {
    doc_format = info->outformat;
  }
  else if (!ippContainsString(doc_formats, "application/pdf"))
  {
    if (ippContainsString(doc_formats, "image/urf"))
      doc_format = "image/urf";
    else if (ippContainsString(doc_formats, "image/pwg-raster"))
      doc_format = "image/pwg-raster";
    else if (ippContainsString(doc_formats, "application/vnd.hp-pcl"))
      doc_format = "application/vnd.hp-pcl";
  }

  // Fetch the job...
  request = ippNewRequest(IPP_OP_FETCH_JOB);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->remote_job_id);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  if (verbosity)
    plogf(NULL, "Connecting to '%s'.", info->printer_uri);

  while ((http = httpConnectURI(info->printer_uri, /*host*/NULL, /*hsize*/0, /*port*/NULL, /*resource*/NULL, /*rsize*/0, /*blocking*/true, /*msec*/30000, /*cancel*/NULL, /*require_ca*/false)) == NULL)
  {
    if (info->done)
      return;

    if (first_time)
      plogf(NULL, "'%s' is not responding, retrying in 15 seconds.", info->printer_uri);

    first_time = false;

    for (int i = 0; i < 15 && !info->done; i ++)
      sleep(1);
  }

  if (verbosity)
    plogf(NULL, "Connected to '%s'.", info->printer_uri);

  job_attrs = cupsDoRequest(http, request, info->resource);

  if (!job_attrs || cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
  {
    // Cannot proxy this job...
    if (cupsGetError() == IPP_STATUS_ERROR_NOT_FETCHABLE)
    {
      plogf(pjob, "Job already fetched by another printer.");
      pjob->local_job_state = IPP_JSTATE_COMPLETED;
      ippDelete(job_attrs);
      return;
    }

    plogf(pjob, "Unable to fetch job: %s", cupsGetErrorString());
    pjob->local_job_state = IPP_JSTATE_ABORTED;
    goto update_job;
  }

  request = ippNewRequest(IPP_OP_ACKNOWLEDGE_JOB);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->remote_job_id);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  ippDelete(cupsDoRequest(http, request, info->resource));

  if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
  {
    plogf(pjob, "Unable to acknowledge job: %s", cupsGetErrorString());
    pjob->local_job_state = IPP_JSTATE_ABORTED;
    goto update_job;
  }

  if ((num_docs = ippGetInteger(ippFindAttribute(job_attrs, "number-of-documents", IPP_TAG_INTEGER), 0)) < 1)
    num_docs = 1;

  plogf(pjob, "Fetched job with %d documents.", num_docs);

  // Then get the document data for each document in the job...
  pjob->local_job_state = IPP_JSTATE_PROCESSING;

  update_job_status(http, info, pjob);

  for (doc_number = 1; doc_number <= num_docs; doc_number ++)
  {
    if (pjob->remote_job_state >= IPP_JSTATE_ABORTED)
      break;

    update_document_status(http, info, pjob, doc_number, IPP_DSTATE_PROCESSING);

    request = ippNewRequest(IPP_OP_FETCH_DOCUMENT);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->remote_job_id);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "document-number", doc_number);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    if (doc_format)
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format-accepted", NULL, doc_format);
//    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "compression-accepted", NULL, "gzip");

    cupsSendRequest(http, request, info->resource, ippGetLength(request));
    doc_attrs = cupsGetResponse(http, info->resource);
    ippDelete(request);

    if (!doc_attrs || cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
    {
      plogf(pjob, "Unable to fetch document #%d: %s", doc_number, cupsGetErrorString());

      pjob->local_job_state = IPP_JSTATE_ABORTED;
      ippDelete(doc_attrs);
      break;
    }

    if (pjob->remote_job_state < IPP_JSTATE_ABORTED)
    {
      // Send document to local printer...
      send_document(http, info, pjob, job_attrs, doc_attrs, doc_number);
    }

    // Acknowledge receipt of the document data...
    ippDelete(doc_attrs);

    request = ippNewRequest(IPP_OP_ACKNOWLEDGE_DOCUMENT);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->remote_job_id);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "document-number", doc_number);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

    ippDelete(cupsDoRequest(http, request, info->resource));
  }

  pjob->local_job_state = IPP_JSTATE_COMPLETED;

  // Update the job state and return...
  update_job:

  ippDelete(job_attrs);

  update_job_status(http, info, pjob);

  httpClose(http);
}


//
// 'run_printer()' - Run the printer until no work remains.
//

static void
run_printer(
    http_t       *http,			// I - Connection to printer
    proxy_info_t *info,			// I - Proxy information
    int          subscription_id)	// I - Subscription ID
{
  ipp_t		*device_attrs,		// Device attributes
		*request,		// IPP request
		*response;		// IPP response
  ipp_attribute_t *attr;		// IPP attribute
  const char	*name,			// Attribute name
		*event;			// Current event
  int		job_id;			// Job ID, if any
  ipp_jstate_t	job_state;		// Job state, if any
  int		seq_number = 1;		// Current event sequence number
  int		get_interval;		// How long to sleep
  cups_thread_t	jobs_thread;		// Job proxy processing thread


  plogf(NULL, "run_printer: info              = %p", (void *)info);
  plogf(NULL, "run_printer: info->done        = %s", info->done ? "true" : "false");
  plogf(NULL, "run_printer: info->printer_uri = \"%s\"", info->printer_uri);
  plogf(NULL, "run_printer: info->resource    = \"%s\"", info->resource);
  plogf(NULL, "run_printer: info->device_uri  = \"%s\"", info->device_uri);
  plogf(NULL, "run_printer: info->device_uuid = \"%s\"", info->device_uuid);
  plogf(NULL, "run_printer: info->outformat   = \"%s\"", info->outformat);

  // Query the printer...
  device_attrs = get_device_attrs(info->device_uri);

  // Setup job processing...
  info->jobs  = cupsArrayNew((cups_array_cb_t)compare_jobs, NULL, NULL, 0, NULL, (cups_afree_cb_t)free);
  jobs_thread = cupsThreadCreate((cups_thread_func_t)proxy_jobs, info);

  // Register the output device...
  if (!update_device_attrs(http, info, device_attrs))
    return;

  if (!update_remote_jobs(http, info))
    return;

  while (!stop_running && !info->done)
  {
    // See if we have any work to do...
    request = ippNewRequest(IPP_OP_GET_NOTIFICATIONS);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "notify-subscription-ids", subscription_id);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "notify-sequence-numbers", seq_number);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    ippAddBoolean(request, IPP_TAG_OPERATION, "notify-wait", false);

    if (verbosity)
      plogipp(/*pjob*/NULL, /*is_request*/true, request);

    response = cupsDoRequest(http, request, info->resource);

    if (verbosity)
      plogipp(/*pjob*/NULL, /*is_request*/false, response);

    if ((attr = ippFindAttribute(response, "notify-get-interval", IPP_TAG_INTEGER)) != NULL)
      get_interval = ippGetInteger(attr, 0);
    else
      get_interval = 10;

    if (verbosity)
      plogf(NULL, "notify-get-interval=%d", get_interval);

    for (attr = ippGetFirstAttribute(response); attr; attr = ippGetNextAttribute(response))
    {
      if (ippGetGroupTag(attr) != IPP_TAG_EVENT_NOTIFICATION || !ippGetName(attr))
        continue;

      event     = NULL;
      job_id    = 0;
      job_state = IPP_JSTATE_PENDING;

      while (ippGetGroupTag(attr) == IPP_TAG_EVENT_NOTIFICATION && (name = ippGetName(attr)) != NULL)
      {
	if (!strcmp(name, "notify-subscribed-event") && ippGetValueTag(attr) == IPP_TAG_KEYWORD)
	{
	  event = ippGetString(attr, 0, NULL);
	}
	else if ((!strcmp(name, "job-id") || !strcmp(name, "notify-job-id")) && ippGetValueTag(attr) == IPP_TAG_INTEGER)
	{
	  job_id = ippGetInteger(attr, 0);
	}
	else if (!strcmp(name, "job-state") && ippGetInteger(attr, 0) > 0)
	{
	  job_state = (ipp_jstate_t)ippGetInteger(attr, 0);
	}
	else if (!strcmp(name, "notify-sequence-number") && ippGetValueTag(attr) == IPP_TAG_INTEGER)
	{
	  int new_seq = ippGetInteger(attr, 0);

	  if (new_seq >= seq_number)
	    seq_number = new_seq + 1;
	}
	else if (!strcmp(name, "printer-state-reasons") && ippContainsString(attr, "identify-printer-requested"))
	{
	  acknowledge_identify_printer(http, info);
        }

        attr = ippGetNextAttribute(response);
      }

      if (event && job_id)
      {
        if (!strcmp(event, "job-fetchable") && job_id)
	{
	  // Queue up new job...
          proxy_job_t *pjob = find_job(info, job_id);

	  if (!pjob)
	  {
	    // Not already queued up, make a new one...
            if ((pjob = (proxy_job_t *)calloc(1, sizeof(proxy_job_t))) != NULL)
            {
              // Add job and then let the proxy thread know we added something...
              pjob->remote_job_id    = job_id;
              pjob->remote_job_state = (int)job_state;
              pjob->local_job_state  = IPP_JSTATE_PENDING;

	      plogf(pjob, "Job is now fetchable, queuing up.", pjob);

              cupsRWLockWrite(&info->jobs_rwlock);
              cupsArrayAdd(info->jobs, pjob);
              cupsRWUnlock(&info->jobs_rwlock);

	      cupsCondBroadcast(&info->jobs_cond);
	    }
	    else
	    {
	      plogf(NULL, "Unable to add job %d to jobs queue.", job_id);
	    }
          }
	}
	else if (!strcmp(event, "job-state-changed") && job_id)
	{
	  // Update our cached job info...  If the job is currently being
	  // proxied and the job has been canceled or aborted, the code will see
	  // that and stop printing locally.
	  proxy_job_t *pjob = find_job(info, job_id);

          if (pjob)
          {
	    pjob->remote_job_state = (int)job_state;

	    plogf(pjob, "Updated remote job-state to '%s'.", ippEnumString("job-state", (int)job_state));

	    cupsCondBroadcast(&info->jobs_cond);
	  }
	}
      }
    }

    // Pause before our next poll of the Infrastructure Printer...
    if (get_interval < 0 || get_interval > 30)
      get_interval = 30;

    while (get_interval > 0 && !stop_running)
    {
      sleep(1);
      get_interval --;
    }

    httpConnectAgain(http, /*msec*/30000, /*cancel*/NULL);
  }

  // Stop the job proxy thread...
  info->done = true;

  cupsCondBroadcast(&info->jobs_cond);
//  cupsThreadCancel(jobs_thread);
  cupsThreadWait(jobs_thread);
}


//
// 'send_document()' - Send a proxied document to the local printer.
//

static void
send_document(http_t       *http,	// I - HTTP connection
              proxy_info_t *info,	// I - Proxy information
              proxy_job_t  *pjob,	// I - Proxy job
              ipp_t        *job_attrs,	// I - Job attributes
              ipp_t        *doc_attrs,	// I - Document attributes
              int          doc_number)	// I - Document number
{
  char		scheme[32],		// URI scheme
		userpass[256],		// URI user:pass
		host[256],		// URI host
		resource[256],		// URI resource
		service[32];		// Service port
  int		port;			// URI port number
  http_addrlist_t *list;		// Address list for socket
  const char	*doc_compression;	// Document compression, if any
  size_t	doc_total = 0;		// Total bytes read
  ssize_t	doc_bytes;		// Bytes read/written
  char		doc_buffer[16384];	// Copy buffer


  if ((doc_compression = ippGetString(ippFindAttribute(doc_attrs, "compression", IPP_TAG_KEYWORD), 0, NULL)) != NULL && !strcmp(doc_compression, "none"))
    doc_compression = NULL;

  if (httpSeparateURI(HTTP_URI_CODING_ALL, info->device_uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host, sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK)
  {
    plogf(pjob, "Invalid device URI '%s'.", info->device_uri);
    pjob->local_job_state = IPP_JSTATE_ABORTED;
    return;
  }

  snprintf(service, sizeof(service), "%d", port);
  if ((list = httpAddrGetList(host, AF_UNSPEC, service)) == NULL)
  {
    plogf(pjob, "Unable to lookup device URI host '%s': %s", host, cupsGetErrorString());
    pjob->local_job_state = IPP_JSTATE_ABORTED;
    return;
  }

  if (!strcmp(scheme, "socket"))
  {
    // AppSocket connection...
    int		sock;			// Output socket

    if (verbosity)
      plogf(pjob, "Connecting to '%s'.", info->device_uri);

    if (!httpAddrConnect(list, &sock, 30000, NULL))
    {
      plogf(pjob, "Unable to connect to '%s': %s", info->device_uri, cupsGetErrorString());
      pjob->local_job_state = IPP_JSTATE_ABORTED;
      httpAddrFreeList(list);
      return;
    }

    if (verbosity)
      plogf(pjob, "Connected to '%s'.", info->device_uri);

    if (doc_compression)
      httpSetField(http, HTTP_FIELD_CONTENT_ENCODING, doc_compression);

    while ((doc_bytes = cupsReadResponseData(http, doc_buffer, sizeof(doc_buffer))) > 0)
    {
      char	*doc_ptr = doc_buffer,	// Pointer into buffer
		*doc_end = doc_buffer + doc_bytes;
					// End of buffer

      doc_total += (size_t)doc_bytes;

      while (doc_ptr < doc_end)
      {
        if ((doc_bytes = write(sock, doc_ptr, (size_t)(doc_end - doc_ptr))) > 0)
          doc_ptr += doc_bytes;
      }
    }

    close(sock);

    plogf(pjob, "Local job created, %ld bytes.", (long)doc_total);
  }
  else
  {
    int			i;		// Looping var
    http_t		*dev_http;	// Device HTTP connection
    http_encryption_t	encryption;	// Encryption mode
    ipp_t		*request,	// IPP request
			*response;	// IPP response
    ipp_attribute_t	*attr;		// Current attribute
    int			create_job = 0;	// Support for Create-Job/Send-Document?
    const char		*doc_format;	// Document format
    ipp_jstate_t	job_state;	// Current job-state value
    static const char * const pattrs[] =// Printer attributes we are interested in
    {
      "compression-supported",
      "operations-supported"
    };
    static const char * const operation[] =
    {					// Operation attributes to copy
      "job-name",
      "job-password",
      "job-password-encryption",
      "job-priority"
    };
    static const char * const job_template[] =
    {					// Job Template attributes to copy
      "copies",
      "finishings",
      "finishings-col",
      "job-account-id",
      "job-accounting-user-id",
      "media",
      "media-col",
      "multiple-document-handling",
      "orientation-requested",
      "page-ranges",
      "print-color-mode",
      "print-quality",
      "sides"
    };

    if ((doc_format = ippGetString(ippFindAttribute(doc_attrs, "document-format", IPP_TAG_MIMETYPE), 0, NULL)) == NULL)
      doc_format = "application/octet-stream";

    // Connect to the IPP/IPPS printer...
    if (port == 443 || !strcmp(scheme, "ipps"))
      encryption = HTTP_ENCRYPTION_ALWAYS;
    else
      encryption = HTTP_ENCRYPTION_IF_REQUESTED;

    if (verbosity)
      plogf(pjob, "Connecting to '%s'.", info->device_uri);

    if ((dev_http = httpConnect(host, port, list, AF_UNSPEC, encryption, 1, 30000, NULL)) == NULL)
    {
      plogf(pjob, "Unable to connect to '%s': %s\n", info->device_uri, cupsGetErrorString());
      httpAddrFreeList(list);
      pjob->local_job_state = IPP_JSTATE_ABORTED;
      return;
    }

    if (verbosity)
      plogf(pjob, "Connected to '%s'.", info->device_uri);

    // See if it supports Create-Job + Send-Document...
    request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->device_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", (int)(sizeof(pattrs) / sizeof(pattrs[0])), NULL, pattrs);

    response = cupsDoRequest(dev_http, request, resource);

    if ((attr = ippFindAttribute(response, "operations-supported", IPP_TAG_ENUM)) == NULL)
    {
      plogf(pjob, "Unable to get list of supported operations from printer.");
      pjob->local_job_state = IPP_JSTATE_ABORTED;
      httpAddrFreeList(list);
      ippDelete(response);
      httpClose(dev_http);
      return;
    }

    create_job = ippContainsInteger(attr, IPP_OP_CREATE_JOB) && ippContainsInteger(attr, IPP_OP_SEND_DOCUMENT);

    if (doc_compression && !ippContainsString(ippFindAttribute(response, "compression-supported", IPP_TAG_KEYWORD), doc_compression))
    {
      // Decompress raster data to send to printer without compression...
      httpSetField(http, HTTP_FIELD_CONTENT_ENCODING, doc_compression);
      doc_compression = NULL;
    }

    ippDelete(response);

    // Create the job and start printing...
    request = ippNewRequest(create_job ? IPP_OP_CREATE_JOB : IPP_OP_PRINT_JOB);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->device_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    if (!create_job)
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", NULL, doc_format);
    if (!create_job && doc_compression)
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "compression", NULL, doc_compression);
    for (i = 0; i < (int)(sizeof(operation) / sizeof(operation[0])); i ++)
    {
      if ((attr = ippFindAttribute(job_attrs, operation[i], IPP_TAG_ZERO)) != NULL)
      {
	attr = ippCopyAttribute(request, attr, 0);
	ippSetGroupTag(request, &attr, IPP_TAG_OPERATION);
      }
    }

    for (i = 0; i < (int)(sizeof(job_template) / sizeof(job_template[0])); i ++)
    {
      if ((attr = ippFindAttribute(job_attrs, job_template[i], IPP_TAG_ZERO)) != NULL)
	ippCopyAttribute(request, attr, 0);
    }

    if (verbosity)
      plogipp(pjob, /*is_request*/true, request);

    if (create_job)
    {
      response = cupsDoRequest(dev_http, request, resource);

      if (verbosity)
        plogipp(pjob, /*is_request*/false, response);

      pjob->local_job_id = ippGetInteger(ippFindAttribute(response, "job-id", IPP_TAG_INTEGER), 0);
      ippDelete(response);

      if (pjob->local_job_id <= 0)
      {
	plogf(pjob, "Unable to create local job: %s", cupsGetErrorString());
	pjob->local_job_state = IPP_JSTATE_ABORTED;
	httpAddrFreeList(list);
	httpClose(dev_http);
	return;
      }

      request = ippNewRequest(IPP_OP_SEND_DOCUMENT);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->device_uri);
      ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->local_job_id);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", NULL, doc_format);
      if (doc_compression)
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "compression", NULL, doc_compression);
      ippAddBoolean(request, IPP_TAG_OPERATION, "last-document", 1);

      if (verbosity)
        plogipp(pjob, /*is_request*/true, request);
    }

    if (cupsSendRequest(dev_http, request, resource, 0) == HTTP_STATUS_CONTINUE)
    {
      while ((doc_bytes = cupsReadResponseData(http, doc_buffer, sizeof(doc_buffer))) > 0)
      {
	doc_total += (size_t)doc_bytes;

        if (cupsWriteRequestData(dev_http, doc_buffer, (size_t)doc_bytes) != HTTP_STATUS_CONTINUE)
          break;
      }
    }

    response = cupsGetResponse(dev_http, resource);

    if (verbosity)
      plogipp(pjob, /*is_request*/false, response);

    if (!pjob->local_job_id)
      pjob->local_job_id = ippGetInteger(ippFindAttribute(response, "job-id", IPP_TAG_INTEGER), 0);

    job_state = (ipp_jstate_t)ippGetInteger(ippFindAttribute(response, "job-state", IPP_TAG_ENUM), 0);

    ippDelete(response);

    if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
    {
      plogf(pjob, "Unable to create local job: %s", cupsGetErrorString());
      pjob->local_job_state = IPP_JSTATE_ABORTED;
      httpAddrFreeList(list);
      httpClose(dev_http);
      return;
    }

    plogf(pjob, "Local job %d created, %ld bytes.", pjob->local_job_id, (long)doc_total);

    while (pjob->remote_job_state < IPP_JSTATE_CANCELED && job_state < IPP_JSTATE_CANCELED)
    {
      request = ippNewRequest(IPP_OP_GET_JOB_ATTRIBUTES);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->device_uri);
      ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->local_job_id);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", NULL, "job-state");

      if (verbosity)
        plogipp(pjob, /*is_request*/true, request);

      response = cupsDoRequest(dev_http, request, resource);

      if (verbosity)
        plogipp(pjob, /*is_request*/false, response);

      if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
	job_state = IPP_JSTATE_COMPLETED;
      else
        job_state = (ipp_jstate_t)ippGetInteger(ippFindAttribute(response, "job-state", IPP_TAG_ENUM), 0);

      ippDelete(response);
    }

    if (pjob->remote_job_state == IPP_JSTATE_CANCELED)
    {
      // Cancel locally...
      plogf(pjob, "Canceling job locally.");

      request = ippNewRequest(IPP_OP_CANCEL_JOB);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->device_uri);
      ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->local_job_id);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

      if (verbosity)
        plogipp(pjob, /*is_request*/true, request);

      response = cupsDoRequest(dev_http, request, resource);

      if (verbosity)
        plogipp(pjob, /*is_request*/false, response);

      ippDelete(response);

      if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
	plogf(pjob, "Unable to cancel local job: %s", cupsGetErrorString());

      pjob->local_job_state = IPP_JSTATE_CANCELED;
    }

    httpClose(dev_http);
  }

  httpAddrFreeList(list);

  update_document_status(http, info, pjob, doc_number, IPP_DSTATE_COMPLETED);
}


//
// 'sighandler()' - Handle termination signals so we can clean up...
//

static void
sighandler(int sig)			// I - Signal
{
  (void)sig;

  stop_running = true;
}


//
// 'update_device_attrs()' - Update device attributes on the server.
//

static bool				// O - `true` on success, `false` on failure
update_device_attrs(
    http_t       *http,			// I - Connection to server
    proxy_info_t *info,			// I - Proxy information
    ipp_t        *new_attrs)		// I - New attributes
{
  int			i,		// Looping var
			result;		// Result of comparison
  ipp_t			*request;	// IPP request
  ipp_attribute_t	*attr;		// New attribute
  const char		*name;		// New attribute name


  // Update the configuration of the output device...
  request = ippNewRequest(IPP_OP_UPDATE_OUTPUT_DEVICE_ATTRIBUTES);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  for (attr = ippGetFirstAttribute(new_attrs); attr; attr = ippGetNextAttribute(new_attrs))
  {
    // Add any attributes that have changed...
    if (ippGetGroupTag(attr) != IPP_TAG_PRINTER || (name = ippGetName(attr)) == NULL)
      continue;

    for (i = 0, result = 1; i < (int)(sizeof(printer_attrs) / sizeof(printer_attrs[0])) && result > 0; i ++)
    {
      if ((result = strcmp(name, printer_attrs[i])) == 0)
      {
        // This is an attribute we care about...
        if (!attrs_are_equal(ippFindAttribute(info->device_attrs, name, ippGetValueTag(attr)), attr))
	  ippCopyAttribute(request, attr, 1);
      }
    }
  }

  ippDelete(cupsDoRequest(http, request, info->resource));

  if (cupsGetError() != IPP_STATUS_OK)
  {
    plogf(NULL, "Unable to update the output device with '%s': %s", info->printer_uri, cupsGetErrorString());
    ippDelete(new_attrs);
    return (false);
  }

  // Save the new attributes...
  ippDelete(info->device_attrs);
  info->device_attrs = new_attrs;

  return (true);
}


//
// 'update_document_status()' - Update the document status.
//

static void
update_document_status(
    http_t       *http,			// I - HTTP connection
    proxy_info_t *info,			// I - Proxy info
    proxy_job_t  *pjob,			// I - Proxy job
    int          doc_number,		// I - Document number
    ipp_dstate_t doc_state)		// I - New document-state value
{
  ipp_t	*request,			// IPP request
	*response;			// IPP response


  request = ippNewRequest(IPP_OP_UPDATE_DOCUMENT_STATUS);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->remote_job_id);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "document-number", doc_number);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  ippAddInteger(request, IPP_TAG_DOCUMENT, IPP_TAG_ENUM, "output-device-document-state", (int)doc_state);

  if (verbosity)
    plogipp(pjob, /*is_request*/true, request);

  response = cupsDoRequest(http, request, info->resource);

  if (verbosity)
    plogipp(pjob, /*is_request*/false, response);

  ippDelete(response);

  if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
    plogf(pjob, "Unable to update the state for document #%d: %s", doc_number, cupsGetErrorString());
}


//
// 'update_job_status()' - Update the job status.
//

static void
update_job_status(http_t       *http,	// I - HTTP connection
                  proxy_info_t *info,	// I - Proxy info
                  proxy_job_t  *pjob)	// I - Proxy job
{
  ipp_t	*request,			// IPP request
	*response;			// IPP response


  request = ippNewRequest(IPP_OP_UPDATE_JOB_STATUS);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->remote_job_id);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  ippAddInteger(request, IPP_TAG_JOB, IPP_TAG_ENUM, "output-device-job-state", (int)pjob->local_job_state);

  if (verbosity)
    plogipp(pjob, /*is_request*/true, request);

  response = cupsDoRequest(http, request, info->resource);

  if (verbosity)
    plogipp(pjob, /*is_request*/false, response);

  ippDelete(response);

  if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
    plogf(pjob, "Unable to update the job state: %s", cupsGetErrorString());
}


//
// 'update_remote_jobs()' - Get the current list of remote, fetchable jobs.
//

static bool				// O - `true` on success, `false` on failure
update_remote_jobs(
    http_t       *http,			// I - Connection to infrastructure printer
    proxy_info_t *info)			// I - Proxy information
{
  ipp_t			*request,	// IPP request
			*response;	// IPP response
  ipp_attribute_t	*attr;		// IPP attribute
  const char		*name;		// Attribute name
  int			job_id;		// Job ID, if any
  ipp_jstate_t		job_state;	// Job state, if any


  // Get the list of fetchable jobs...
  plogf(NULL, "Getting fetchable jobs...");
  request = ippNewRequest(IPP_OP_GET_JOBS);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
  ippAddString(request, IPP_TAG_OPERATION, IPP_CONST_TAG(IPP_TAG_KEYWORD), "which-jobs", NULL, "fetchable");
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);

  if (verbosity)
    plogipp(/*pjob*/NULL, /*is_request*/true, request);

  if ((response = cupsDoRequest(http, request, info->resource)) == NULL)
  {
    plogf(NULL, "Get-Jobs failed: %s", cupsGetErrorString());
    return (false);
  }

  if (verbosity)
    plogipp(/*pjob*/NULL, /*is_request*/false, response);

  // Scan the list...
  for (attr = ippGetFirstAttribute(response); attr; attr = ippGetNextAttribute(response))
  {
    // Skip to the start of the next job group...
    while (attr && ippGetGroupTag(attr) != IPP_TAG_JOB)
      attr = ippGetNextAttribute(response);
    if (!attr)
      break;

    // Get the job-id and state...
    job_id    = 0;
    job_state = IPP_JSTATE_PENDING;

    while (attr && ippGetGroupTag(attr) == IPP_TAG_JOB)
    {
      name = ippGetName(attr);
      if (!strcmp(name, "job-id"))
        job_id = ippGetInteger(attr, 0);
      else if (!strcmp(name, "job-state"))
        job_state = (ipp_jstate_t)ippGetInteger(attr, 0);

      attr = ippGetNextAttribute(response);
    }

    if (job_id && (job_state == IPP_JSTATE_PENDING || job_state == IPP_JSTATE_STOPPED))
    {
      // Add this job...
      proxy_job_t *pjob = find_job(info, job_id);

      if (!pjob)
      {
        // Not already queued up, make a new one...
	if ((pjob = (proxy_job_t *)calloc(1, sizeof(proxy_job_t))) != NULL)
	{
	  // Add job and then let the proxy thread know we added something...
	  pjob->remote_job_id    = job_id;
	  pjob->remote_job_state = (int)job_state;
	  pjob->local_job_state  = IPP_JSTATE_PENDING;

	  plogf(pjob, "Job is now fetchable, queuing up.", pjob);

	  cupsRWLockWrite(&info->jobs_rwlock);
	  cupsArrayAdd(info->jobs, pjob);
	  cupsRWUnlock(&info->jobs_rwlock);

	  cupsCondBroadcast(&info->jobs_cond);
	}
	else
	{
	  plogf(NULL, "Unable to add job %d to jobs queue.", job_id);
	}
      }
    }
  }

  ippDelete(response);

  return (true);
}


//
// 'usage()' - Show program usage and exit.
//

static int				// O - Exit status
usage(FILE *out)			// I - Output file
{
  fputs("Usage: ippproxy [OPTIONS] PRINTER-URI\n", out);
  fputs("Options:\n", out);
  fputs("  -d DEVICE-URI   Specify local printer device URI.\n", out);
  fputs("  -m MIME/TYPE    Specify the desired print format.\n", out);
  fputs("  -p PASSWORD     Password for authentication.\n", out);
  fputs("                  (Also IPPPROXY_PASSWORD environment variable)\n", out);
  fputs("  -u USERNAME     Username for authentication.\n", out);
  fputs("  -v              Be verbose.\n", out);
  fputs("  --help          Show this help.\n", out);
  fputs("  --version       Show program version.\n", out);

  return (out == stderr ? 1 : 0);
}
