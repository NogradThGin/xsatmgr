/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>

#define VERSION_STRING "alpha-v3"

#define LUT_SIZE 4096

#define PROP_CTM "CTM"

/**
 * The below data structures are identical to the ones used by DRM. They are
 * here to help us structure the data being passed to the kernel.
 */
struct _drm_color_ctm {
	/* Transformation matrix in S31.32 format. */
	int64_t matrix[9];
};

/*******************************************************************************
 * Helper functions
 */

/**
 * Translate coefficients to a color CTM format that DRM accepts.
 *
 * DRM requres the CTM to be in signed-magnitude, not 2's complement.
 * It is also in 31.32 fixed-point format.
 *
 * @coeffs: Input coefficients
 * @ctm: DRM CTM struct, used to create the blob. The translated values will be
 *       placed here.
 */
static void coeffs_to_ctm(double *coeffs,
			  struct _drm_color_ctm *ctm)
{
	int i;
	for (i = 0; i < 9; i++) {
		if (coeffs[i] < 0) {
			ctm->matrix[i] =
				(int64_t) (-coeffs[i] * ((int64_t) 1L << 32));
			ctm->matrix[i] |= 1ULL << 63;
		} else
			ctm->matrix[i] =
				(int64_t) (coeffs[i] * ((int64_t) 1L << 32));
	}
}

/**
 * Find the output on the RandR screen resource by name.
 *
 * dpy: The X display
 * res: The RandR screen resource
 * name: The output name to search for.
 *
 * Return: The RROutput X-id if found, 0 (None) otherwise.
 */
static RROutput find_output_by_name(Display *dpy, XRRScreenResources *res,
				    const char *name)
{
	int i;
	RROutput ret;
	XRROutputInfo *output_info;

	for (i = 0; i < res->noutput; i++) {
		ret = res->outputs[i];
		output_info = XRRGetOutputInfo (dpy, res, ret);

		if (!strcmp(name, output_info->name)) {
			XRRFreeOutputInfo(output_info);
			return ret;
		}

		XRRFreeOutputInfo(output_info);
	}
	return 0;
}

enum randr_format {
    FORMAT_16_BIT = 16,
    FORMAT_32_BIT = 32,
};

/**
 * Set a DRM blob property on the given output. It calls XSync at the end to
 * flush the change request so that it applies.
 *
 * @dpy: The X Display
 * @output: RandR output to set the property on
 * @prop_name: String name of the property.
 * @blob_data: The data of the property blob.
 * @blob_bytes: Size of the data, in bytes.
 * @format: Format of each element within blob_data.
 *
 * Return: X-defined return codes:
 *     - BadAtom if the given name string doesn't exist.
 *     - BadName if the property referenced by the name string does not exist on
 *       the given connector
 *     - Success otherwise.
 */
static int set_output_blob(Display *dpy, RROutput output,
			   const char *prop_name, void *blob_data,
			   size_t blob_bytes, enum randr_format format)
{
	Atom prop_atom;
	XRRPropertyInfo *prop_info;

	/* Find the X Atom associated with the property name */
	prop_atom = XInternAtom (dpy, prop_name, 1);
	if (!prop_atom) {
		printf("Property key '%s' not found.\n", prop_name);
		return BadAtom;
	}

	/* Make sure the property exists */
	prop_info = XRRQueryOutputProperty(dpy, output, prop_atom);
	if (!prop_info) {
		printf("Property key '%s' not found on output\n", prop_name);
		return BadName;  /* Property not found */
	}

	/* Change the property 
	 *
	 * Due to some restrictions in RandR, array properties of 32-bit format
	 * must be of type 'long'. See set_ctm() for details.
	 *
	 * To get the number of elements within blob_data, we take its size in
	 * bytes, divided by the size of one of it's elements in bytes:
	 *
	 * blob_length = blob_bytes / (element_bytes)
	 *             = blob_bytes / (format / 8)
	 *             = blob_bytes / (format >> 3)
	 */
	XRRChangeOutputProperty(dpy, output, prop_atom,
				XA_INTEGER, format, PropModeReplace,
				blob_data, blob_bytes / (format >> 3));
	/* Call XSync to apply it. */
	XSync(dpy, 0);

	return Success;
}

/**
 * Set the de/regamma LUT. Since setting degamma and regamma follows similar
 * procedures, a flag is used to determine which one is set. Also note the
 * special case of setting SRGB gamma, explained further below.
 *
 * @dpy: The X display
 * @output: The output on which to set de/regamma on.
 * @coeffs: Coefficients used to create the DRM color LUT blob.
 * @is_srgb: True if SRGB gamma is being programmed. This is a special case,
 *           since amdgpu DC defaults to SRGB when no DRM blob (i.e. NULL) is
 *           set. In other words, there is no need to create a blob (just set
 *           the blob id to 0)
 * @is_degamma: True if degamma is being set. Set regamma otherwise.
 */

/**
 * Create a DRM color transform matrix using the given coefficients, and set
 * the output's CRTC to use it.
 */
static int set_ctm(Display *dpy, RROutput output, double *coeffs)
{
	size_t blob_size = sizeof(struct _drm_color_ctm);
	struct _drm_color_ctm ctm;
	long padded_ctm[18];

	int i, ret;

	coeffs_to_ctm(coeffs, &ctm);

	/* Workaround:
	 *
	 * RandR currently uses long types for 32-bit integer format. However,
	 * 64-bit systems will use 64-bits for long, causing data corruption
	 * once RandR parses the data. Therefore, pad the blob_data to be long-
	 * sized. This will work regardless of how long is defined (as long as
	 * it's at least 32-bits).
	 *
	 * Note that we have a 32-bit format restriction; we have to interpret
	 * each S31.32 fixed point number within the CTM in two parts: The
	 * whole part (S31), and the fractional part (.32). They're then stored
	 * (as separate parts) into a long-typed array. Of course, This problem
	 * wouldn't exist if xserver accepted 64-bit formats.
	 *
	 * A gotcha here is the endianness of the S31.32 values. The whole part
	 * will either come before or after the fractional part. (before in
	 * big-endian format, and after in small-endian format). We could avoid
	 * dealing with this by doing a straight memory copy, but we have to
	 * ensure that each 32-bit element is padded to long-size in the
	 * process.
	 */
	for (i = 0; i < 18; i++)
		/* Think of this as a padded 'memcpy()'. */
		padded_ctm[i] = ((uint32_t*)ctm.matrix)[i];

	ret = set_output_blob(dpy, output, PROP_CTM, &padded_ctm,
			      blob_size, FORMAT_32_BIT);

	if (ret)
		printf("Failed to set CTM. %d\n", ret);
	return ret;
}

/*******************************************************************************
 * main function, and functions to assist in parsing input.
 */

/**
 * Parse user input, and fill the coefficients array with the requested CTM.
 *
 * @ctm_opt: user input
 * @coeffs: Array of 9 doubles. The requested CTM will be filled in here.
 *
 * Return: True if user has requested CTM change. False otherwise.
 */
int parse_user_ctm(char *ctm_opt, double *coeffs)
{
	if (!ctm_opt)
        return 0;

    if (!strcmp(ctm_opt, "default")) {
        printf("Using identity CTM\n");
        double temp[9] = {
            1, 0, 0,
            0, 1, 0,
            0, 0, 1
        };
        memcpy(coeffs, temp, sizeof(double) * 9);
        return 1;
    }

    double value = strtod(ctm_opt, NULL);
    if(!value) {
        printf("%s is not a valid Saturation value. Skipping.\n",
               ctm_opt);
        return 0;
    }


    double s = (1.0 - value) / 3.0;
    double temp[9] = {
        s + value, s, s,
        s, s + value, s,
        s, s, s + value
    };


    printf("Using custom CTM:\n");
    printf("    %2.4f:%2.4f:%2.4f\n", temp[0], temp[1], temp[2]);
    printf("    %2.4f:%2.4f:%2.4f\n", temp[3], temp[4], temp[5]);
    printf("    %2.4f:%2.4f:%2.4f\n", temp[6], temp[7], temp[8]);



    memcpy(coeffs, temp, sizeof(double) * 9);
    return 1;
}

/**
 * Parse user input, and fill the coefficients array with the requested LUT.
 * If predefined SRGB LUT is requested, the coefficients array is not touched,
 * and is_srgb is set to true. See set_gamma() for why.
 *
 * @gamma_opt: User input
 * @coeffs: Array of color3d structs. The requested LUT will be filled in here.
 * @is_srgb: Will be set to true if user requested SRGB LUT.
 *
 * Return: True if user has requested gamma change. False otherwise.
 */

static char HELP_STR[] = { 
	#include "help.xxd"
};

static void print_short_help()
{
	/* Just get first line, up to the first new line.*/
	uint32_t len = strlen(HELP_STR);
	char *short_help;

	int i;
	for (i = 0; i < len; i++) {
		if (HELP_STR[i] == '\n')
			break;
	}

	short_help = malloc(sizeof(char) * i);
	strncpy(short_help, HELP_STR, i);

	printf("%s\n", short_help);
	free(short_help);
}

static void print_version()
{
	printf("%s\n", VERSION_STRING);
}



int main(int argc, char *const argv[])
{
	/* These coefficient arrays store an intermediary form of the property 
	 * blob to be set. They will be translated into the format that DDX
	 * driver expects when the request is sent to XRandR.
	 */
	double ctm_coeffs[9];

	int ret = 0;

	/* Things needed by xrandr to change output properties */
	Display *dpy;
	Window root;
	XRRScreenResources *res;
	RROutput output;


	/*
	 * Parse arguments
	 */
	int opt = -1;
	char *ctm_opt = NULL;
	char *output_name = NULL;

	int ctm_changed;

    while ((opt = getopt(argc, argv, "vho:c:")) != -1) {
		if (opt == 'v') {
			print_version();
			return 0;
		}
		else if (opt == 'c')
			ctm_opt = optarg;
		else if (opt == 'o')
			output_name = optarg;
		else if (opt == 'h') {
			printf("%s", HELP_STR);
			return 0;
		}
		else {
			print_short_help();
			return 1;
		}
	}

	/* Check that output is given */
	if (!output_name) {
		print_short_help();
		return 1;
	}

	/* Parse the input, and generate the intermediate coefficient arrays */
	ctm_changed = parse_user_ctm(ctm_opt, ctm_coeffs);


	/* Print help if input is not as expected */
    if (!ctm_changed) {
		print_short_help();
		return 1;
	}

	/* Open the default X display and window, then obtain the RandR screen
	 * resource. Note that the DISPLAY environment variable must exist. */
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		printf("No display specified, check the DISPLAY environment "
		       "variable.\n");
		return 1;
	}

	root = DefaultRootWindow(dpy);
	res = XRRGetScreenResourcesCurrent(dpy, root);

	/* RandR needs to know which output we're setting the property on.
	 * Since we only have a name to work with, find the RROutput using the
	 * name. */
	output = find_output_by_name(dpy, res, output_name);
	if (!output) {
		printf("Cannot find output %s.\n", output_name);
		ret = 1;
		goto done;
	}

	/* Set the properties as parsed. The set_* functions will also
	 * translate the coefficients. */
	if (ctm_changed) {
        ret = set_ctm(dpy, output, ctm_coeffs);
		if (ret)
			goto done;
	}

done:
	/* Ensure proper cleanup */
	XRRFreeScreenResources(res);
	XCloseDisplay(dpy);

	return ret;
}
