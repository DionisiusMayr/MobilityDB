/*****************************************************************************
 *
 * This MobilityDB code is provided under The PostgreSQL License.
 * Copyright (c) 2016-2023, Université libre de Bruxelles and MobilityDB
 * contributors
 *
 * MobilityDB includes portions of PostGIS version 3 source code released
 * under the GNU General Public License (GPLv2 or later).
 * Copyright (c) 2001-2023, PostGIS contributors
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written
 * agreement is hereby granted, provided that the above copyright notice and
 * this paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL UNIVERSITE LIBRE DE BRUXELLES BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION,
 * EVEN IF UNIVERSITE LIBRE DE BRUXELLES HAS BEEN ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * UNIVERSITE LIBRE DE BRUXELLES SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE. THE SOFTWARE PROVIDED HEREUNDER IS ON
 * AN "AS IS" BASIS, AND UNIVERSITE LIBRE DE BRUXELLES HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 *****************************************************************************/

/**
 * @brief A simple program that reads from a CSV file synthetic trip data in
 * Brussels generated by the MobilityDB-BerlinMOD generator
 * https://github.com/MobilityDB/MobilityDB-BerlinMOD
 * and generate statics about the Brussels communes (or municipalities)
 * traversed by the trips.
 *
 * The input files are
 * - `communes.csv`: data from the 19 communes composing Brussels obtained from
 *   OSM and publicly available statistical data
 * - `brussels_region.csv`: geometry of the Brussels region obtained from OSM.
 *   It is the spatial union of the 19 communes
 * - `trips.csv`: 55 trips from 5 cars during 4 days obtained from the
 *   generator at scale factor 0.005
 * In the above files, the coordinates are given in the 3857 coordinate system,
 * https://epsg.io/3857
 * and the timestamps are given in the Europe/Brussels time zone.
 * This simple program does not cope with erroneous inputs, such as missing
 * fields or invalid values.
 *
 * The program can be build as follows
 * @code
 * gcc -Wall -g -I/usr/local/include -o 06_meos_clip_berlinmod 06_meos_clip_berlinmod.c -L/usr/local/lib -lmeos
 * @endcode
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <meos.h>

/* Maximum length in characters of a trip in the input data */
#define MAX_LENGTH_TRIP 170001
/* Maximum length in characters of a geometry in the input data */
#define MAX_LENGTH_GEOM 100000
/* Maximum length in characters of a header record in the input CSV file */
#define MAX_LENGTH_HEADER 1024
/* Maximum length in characters of a name in the input data */
#define MAX_LENGTH_NAME 100
/* Maximum length in characters of a date in the input data */
#define MAX_LENGTH_DATE 12
/* Number of vehicles */
#define NO_VEHICLES 5
/* Number of communes */
#define NO_COMMUNES 19

typedef struct
{
  int id;
  char name[MAX_LENGTH_NAME];
  int population;
  GSERIALIZED *geom;
} commune_record;

typedef struct
{
  char name[MAX_LENGTH_NAME];
  GSERIALIZED *geom;
} region_record;

typedef struct
{
  int tripid;
  int vehid;
  int seq;
  Temporal *trip;
} trip_record;

/* Arrays to compute the results */
commune_record communes[NO_COMMUNES];
double distance[NO_VEHICLES + 1][NO_COMMUNES + 3] = {0};

char trip_buffer[MAX_LENGTH_TRIP];
char geo_buffer[MAX_LENGTH_GEOM];
char header_buffer[MAX_LENGTH_HEADER];
char date_buffer[MAX_LENGTH_DATE];

region_record brussels_region;

/* Read communes from file */
int read_communes(void)
{
  /* You may substitute the full file path in the first argument of fopen */
  FILE *file = fopen("communes.csv", "r");

  if (! file)
  {
    printf("Error opening file\n");
    return 1;
  }

  int no_records = 0;

  /* Read the first line of the file with the headers */
  fscanf(file, "%1023s\n", header_buffer);

  /* Continue reading the file */
  do
  {
    int read = fscanf(file, "%d,%100[^,],%d,%100000[^\n]\n",
      &communes[no_records].id, communes[no_records].name,
      &communes[no_records].population, geo_buffer);
    /* Transform the string representing the geometry into a geometry value */
    communes[no_records++].geom = gserialized_in(geo_buffer, -1);

    if (read != 4 && !feof(file))
    {
      printf("Commune record with missing values\n");
      return 1;
    }

    if (ferror(file))
    {
      printf("Error reading file\n");
      return 1;
    }
  } while (!feof(file));

  printf("%d commune records read\n", no_records);

  /* Close the file */
  fclose(file);

  return 0;
}

/* Read Brussels region from file */
int read_brussels_region(void)
{
  /* You may substitute the full file path in the first argument of fopen */
  FILE *file = fopen("brussels_region.csv", "r");

  if (! file)
  {
    printf("Error opening file\n");
    return 1;
  }

  /* Read the first line of the file with the headers */
  fscanf(file, "%1023s\n", header_buffer);

  /* Continue reading the file */
  int read = fscanf(file, "%100[^,],%100000[^\n]\n", brussels_region.name,
    geo_buffer);
  /* Transform the string representing the geometry into a geometry value */
  brussels_region.geom = gserialized_in(geo_buffer, -1);

  if (read != 2 && !feof(file))
  {
    printf("Region record with missing values\n");
    fclose(file);
    return 1;
  }

  if (ferror(file))
  {
    printf("Error reading file\n");
    fclose(file);
    return 1;
  }

  printf("Brussels region record read\n");

  /* Close the file */
  fclose(file);

  return 0;
}

/**
 * Print a distance matrix in tabular form
 */
void
matrix_print(double distance[NO_VEHICLES + 1][NO_COMMUNES + 3],
  bool all_communes)
{
  int len = 0;
  char buf[65536];
  int i, j;

  /* Print table header */
  len += sprintf(buf+len, "\n                --");
  for (j = 1; j < NO_COMMUNES + 2; j++)
  {
    if (all_communes || distance[NO_VEHICLES][j] != 0)
      len += sprintf(buf+len, "---------");
  }
  len += sprintf(buf+len, "\n                | Commmunes");
  len += sprintf(buf+len, "\n    --------------");
  for (j = 1; j < NO_COMMUNES + 2; j++)
  {
    if (all_communes || distance[NO_VEHICLES][j] != 0)
      len += sprintf(buf+len, "---------");
  }
  len += sprintf(buf+len, "\nVeh | Distance | ");
  for (j = 1; j < NO_COMMUNES + 1; j++)
  {
    if (all_communes || distance[NO_VEHICLES][j] != 0)
      len += sprintf(buf+len, "   %2d   ", j);
  }
  len += sprintf(buf+len, "|  Inside | Outside\n");
  for (j = 0; j < NO_COMMUNES + 3; j++)
  {
    if (all_communes || distance[NO_VEHICLES][j] != 0)
      len += sprintf(buf+len, "---------");
  }
  len += sprintf(buf+len, "\n");

  /* Print for each vehicle */
  for (i = 0; i < NO_VEHICLES; i++)
  {
    /* Print the vehicle number and the total distance for the vehicle */
    len += sprintf(buf+len, " %2d | %8.3f |", i + 1, distance[i][0]);
    /* Print the total distance per commune for the vehicle */
    for (j = 1; j <= NO_COMMUNES; j++)
    {
      if (all_communes || distance[NO_VEHICLES][j] != 0)
      {
        len += sprintf(buf+len, " %7.3f", distance[i][j]);
      }
    }
    /* Print the total distance outside and inside Brussels for the vehicle */
    for (j = NO_COMMUNES + 1; j < NO_COMMUNES + 3; j++)
      len += sprintf(buf+len, " | %7.3f", distance[i][j]);
    len += sprintf(buf+len, "\n");
  }

  /* Print the total row */
  for (j = 0; j < NO_COMMUNES + 3; j++)
  {
    if (all_communes || distance[NO_VEHICLES][j] != 0)
      len += sprintf(buf+len, "---------");
  }
  len += sprintf(buf+len, "\n    | %8.3f |", distance[NO_VEHICLES][0]);
  /* Print the total distance per commune */
  for (j = 1; j <= NO_COMMUNES; j++)
  {
    if (all_communes || distance[NO_VEHICLES][j] != 0)
      len += sprintf(buf+len, " %7.3f", distance[NO_VEHICLES][j]);
  }
  /* Print the total distance outside and inside Brussels */
  for (j = NO_COMMUNES + 1; j < NO_COMMUNES + 3; j++)
  {
    if (all_communes || distance[NO_VEHICLES][j] != 0)
      len += sprintf(buf+len, " | %7.3f", distance[NO_VEHICLES][j]);
  }
  len += sprintf(buf+len, "\n");
  for (j = 0; j < NO_COMMUNES + 3; j++)
  {
    if (all_communes || distance[NO_VEHICLES][j] != 0)
      len += sprintf(buf+len, "---------");
  }
  sprintf(buf+len, "\n\n");
  printf("%s", buf);

  return;
}

/* Main program */
int main(void)
{
  /* Initialize MEOS */
  meos_initialize(NULL);

  /* Read communes file */
  read_communes();

  /* Read communes file */
  read_brussels_region();

  /* You may substitute the full file path in the first argument of fopen */
  FILE *file = fopen("trips.csv", "r");

  if (! file)
  {
    printf("Error opening file\n");
    return 1;
  }

  trip_record trip_rec;
  int no_records = 0, i;

  /* Read the first line of the file with the headers */
  fscanf(file, "%1023s\n", header_buffer);
  printf("Processing trip records (one marker per trip)\n");

  /* Continue reading the file */
  do
  {
    int read = fscanf(file, "%d,%d,%10[^,],%d,%170000[^\n]\n",
      &trip_rec.tripid, &trip_rec.vehid, date_buffer, &trip_rec.seq,
      trip_buffer);
    /* Transform the string representing the trip into a temporal value */
    trip_rec.trip = temporal_from_hexwkb(trip_buffer);

    if (read == 5)
    {
      no_records++;
      printf("*");
      fflush(stdout);
    }

    if (read != 5 && !feof(file))
    {
      printf("Trip record with missing values\n");
      fclose(file);
      return 1;
    }

    if (ferror(file))
    {
      printf("Error reading file\n");
      fclose(file);
      return 1;
    }

    /* Compute the total distance */
    double d = tpoint_length(trip_rec.trip) / 1000;
    /* Add to the vehicle total and the column total */
    distance[trip_rec.vehid - 1][0] += d;
    distance[NO_VEHICLES][0] += d;
    /* Loop for each commune */
    for (i = 0; i < NO_COMMUNES; i ++)
    {
      Temporal *atgeom = tpoint_at_geom_time(trip_rec.trip,
        communes[i].geom, NULL, NULL);
      if (atgeom)
      {
        /* Compute the length of the trip projected to the commune */
        d = tpoint_length(atgeom) / 1000;
        /* Add to the cell */
        distance[trip_rec.vehid - 1][i + 1] += d;
        /* Add to the row total, the commune total, and inside total */
        distance[trip_rec.vehid - 1][NO_COMMUNES + 1] += d;
        distance[NO_VEHICLES][i + 1] += d;
        distance[NO_VEHICLES][NO_COMMUNES + 1] += d;
        free(atgeom);
      }
    }
    /* Compute the distance outside Brussels Region */
    Temporal *minusgeom = tpoint_minus_geom_time(trip_rec.trip,
      brussels_region.geom, NULL, NULL);
    if (minusgeom)
    {
      d = tpoint_length(minusgeom) / 1000;
      /* Add to the row */
      distance[trip_rec.vehid - 1][NO_COMMUNES + 2] += d;
      /* Add to the column total */
      distance[NO_VEHICLES][NO_COMMUNES + 2] += d;
      free(minusgeom);
    }

    /* Free memory */
    free(trip_rec.trip);

  } while (!feof(file));

  printf("\n%d trip records read.\n\n", no_records);
  /* The last argument states whether all communes, including those that have
     a zero value, are printed */
  matrix_print(distance, false);

  /* Free memory */
  for (i = 0; i < NO_COMMUNES; i++)
    free(communes[i].geom);
  free(brussels_region.geom);

  /* Close the file */
  fclose(file);

  /* Finalize MEOS */
  meos_finalize();

  return 0;
}
